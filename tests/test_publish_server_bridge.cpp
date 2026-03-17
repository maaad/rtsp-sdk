#include <rtsp-client/rtsp-client.h>
#include <rtsp-publisher/rtsp-publisher.h>
#include <rtsp-common/socket.h>
#include <rtsp-server/rtsp-server.h>

#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

using namespace rtsp;

namespace {

std::string sendRawRtspRequest(uint16_t port, const std::string& request) {
    Socket socket;
    if (!socket.connect("127.0.0.1", port, 2000)) {
        return "";
    }
    if (socket.send(reinterpret_cast<const uint8_t*>(request.data()), request.size()) <= 0) {
        return "";
    }
    uint8_t buffer[4096];
    const ssize_t n = socket.recv(buffer, sizeof(buffer), 2000);
    if (n <= 0) {
        return "";
    }
    return std::string(reinterpret_cast<const char*>(buffer), static_cast<size_t>(n));
}

void test_invalid_announce_rejected() {
    std::cout << "Testing invalid ANNOUNCE rejection..." << std::endl;

    RtspServer server;
    assert(server.init("127.0.0.1", 19771));
    assert(server.start());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const std::string bad_sdp =
        "v=0\r\n"
        "o=- 1 1 IN IP4 127.0.0.1\r\n"
        "s=BadPublish\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "t=0 0\r\n"
        "m=video 0 RTP/AVP 96\r\n"
        "a=control:streamid=0\r\n";

    std::ostringstream req;
    req << "ANNOUNCE rtsp://127.0.0.1:19771/live/bad RTSP/1.0\r\n";
    req << "CSeq: 1\r\n";
    req << "Content-Type: application/sdp\r\n";
    req << "Content-Length: " << bad_sdp.size() << "\r\n\r\n";
    req << bad_sdp;

    const std::string resp = sendRawRtspRequest(19771, req.str());
    assert(resp.find("400 Bad Request") != std::string::npos);

    server.stop();
    std::cout << "invalid ANNOUNCE rejection passed!" << std::endl;
}

} // namespace

int main() {
    std::cout << "Testing publisher -> builtin server -> client bridge..." << std::endl;

    RtspServer server;
    assert(server.init("127.0.0.1", 19770));
    assert(server.start());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    RtspPublisher publisher;
    RtspPublishConfig publish_config;
    publish_config.local_rtp_port = 25060;
    publisher.setConfig(publish_config);
    assert(publisher.open("rtsp://127.0.0.1:19770/live/publish"));

    PublishMediaInfo media;
    media.codec = CodecType::H264;
    media.payload_type = 96;
    media.width = 640;
    media.height = 480;
    media.fps = 25;
    media.sps = {0x67, 0x42, 0x00, 0x28};
    media.pps = {0x68, 0xCE, 0x3C, 0x80};
    media.control_track = "streamid=0";
    assert(publisher.announce(media));
    assert(publisher.setup());
    assert(publisher.record());

    RtspClient client;
    RtspClientConfig client_config;
    client_config.prefer_tcp_transport = true;
    client_config.fallback_to_tcp = true;
    client.setConfig(client_config);
    assert(client.open("rtsp://127.0.0.1:19770/live/publish"));
    assert(client.describe());
    assert(client.setup(0));
    assert(client.play(0));

    const std::vector<std::uint8_t> idr = {
        0x00, 0x00, 0x00, 0x01,
        0x67, 0x42, 0x00, 0x28,
        0x00, 0x00, 0x00, 0x01,
        0x68, 0xCE, 0x3C, 0x80,
        0x00, 0x00, 0x00, 0x01,
        0x65, 0x88, 0x84, 0x21
    };

    VideoFrame frame{};
    bool got_frame = false;
    for (int i = 0; i < 20 && !got_frame; ++i) {
        assert(publisher.pushH264Data(idr.data(), idr.size(), static_cast<std::uint64_t>(i * 40), true));
        got_frame = client.receiveFrame(frame, 200);
    }

    assert(got_frame);
    assert(frame.codec == CodecType::H264);
    assert(frame.size == idr.size());
    assert(frame.width == 640);
    assert(frame.height == 480);

    const RtspServerStats stats = server.getStats();
    assert(stats.frames_pushed >= 1);
    assert(stats.sessions_created >= 2);

    client.close();
    publisher.close();
    server.stop();

    std::cout << "publisher -> builtin server -> client bridge passed!" << std::endl;

    test_invalid_announce_rejected();
    return 0;
}
