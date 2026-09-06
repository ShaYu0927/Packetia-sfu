#include <gtest/gtest.h>
#include "UdpMediaTransport.h"
#include "RtspServer.h"
#include "RtspUtil.h"
#include "IWorkerModule.h"
#include "core/EncodedFrameRouter.h"

#include <array>
#include <condition_variable>
#include <future>
#include <poll.h>

namespace {
using namespace std::chrono_literals;
using media::transport::UdpMediaTransport;
using network::SocketAddr;

class FakeDatagram : public network::transport::IDatagramTransport {
public:
    uint64_t Id() const noexcept override { return 1; }
    bool IsWritable() const noexcept override { return true; }
    network::transport::DatagramSendResult SendDatagram(const SocketAddr& peer,
                                                       const uint8_t* data, size_t size) override {
        last_peer = peer;
        sent.assign(data, data + size);
        return network::transport::DatagramSendResult::Ok;
    }
    void SetDatagramSink(std::weak_ptr<network::transport::IDatagramSink> value) override { sink = value; }
    void Close() override {}
    void Deliver(const SocketAddr& peer, const std::vector<uint8_t>& data) {
        if (auto target = sink.lock()) target->OnDatagram({Id(), 1, peer, data.data(), data.size()});
    }
    std::weak_ptr<network::transport::IDatagramSink> sink;
    SocketAddr last_peer;
    std::vector<uint8_t> sent;
};

class PacketCollector : public IMediaPacketSink {
public:
    MediaPacketIngressResult OnMediaPacket(ReceivedMediaPacket packet) override {
        std::lock_guard<std::mutex> lock(mutex);
        packets.push_back(std::move(packet));
        cv.notify_all();
        return MediaPacketIngressResult::Accepted;
    }
    bool Wait(size_t count) {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, 2s, [&] { return packets.size() >= count; });
    }
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<ReceivedMediaPacket> packets;
};

const std::vector<uint8_t> kRtp{0x80, 0xE0, 0, 16, 0, 1, 0x5F, 0x90, 0x11, 0x22, 0x33, 0x44, 0x65, 0xAA, 0xBB};
const std::vector<uint8_t> kRtcp{0x80, 201, 0, 1, 0x11, 0x22, 0x33, 0x44};

TEST(UdpBinding, DedicatedRtpPortDoesNotGuessTypeFromSecondByte) {
    auto rtp = std::make_shared<FakeDatagram>();
    auto rtcp = std::make_shared<FakeDatagram>();
    auto transport = std::make_shared<UdpMediaTransport>(11, rtp, rtcp);
    auto sink = std::make_shared<PacketCollector>();
    auto peer = SocketAddr::FromIPPort("127.0.0.1", 5000);
    transport->SetSelectedPeers(peer, peer);
    transport->SetPacketSink(sink);
    ASSERT_TRUE(transport->Start());
    auto packet = kRtp;
    packet[1] = 200; // Marker + PT 72 is legal on dedicated RTP, ambiguous in mux.
    rtp->Deliver(peer, packet);
    rtcp->Deliver(peer, kRtcp);
    ASSERT_EQ(sink->packets.size(), 2U);
    EXPECT_EQ(sink->packets[0].type, MediaPacketType::Rtp);
    EXPECT_EQ(sink->packets[1].type, MediaPacketType::Rtcp);
    EXPECT_EQ(sink->packets[0].transport_id, 11U);
}

TEST(UdpBinding, OutboundRoleChoosesCorrespondingSocketAndPeer) {
    auto rtp = std::make_shared<FakeDatagram>();
    auto rtcp = std::make_shared<FakeDatagram>();
    auto transport = std::make_shared<UdpMediaTransport>(12, rtp, rtcp);
    auto rtp_peer = SocketAddr::FromIPPort("127.0.0.1", 5000);
    auto rtcp_peer = SocketAddr::FromIPPort("127.0.0.1", 5001);
    transport->SetSelectedPeers(rtp_peer, rtcp_peer);
    ASSERT_TRUE(transport->Start());
    EXPECT_EQ(transport->SendRtp(kRtp.data(), kRtp.size()), SendResult::Ok);
    EXPECT_EQ(transport->SendRtcp(kRtcp.data(), kRtcp.size()), SendResult::Ok);
    EXPECT_EQ(rtp->last_peer, rtp_peer);
    EXPECT_EQ(rtcp->last_peer, rtcp_peer);
    EXPECT_EQ(rtp->sent, kRtp);
    EXPECT_EQ(rtcp->sent, kRtcp);
}

TEST(UdpBinding, ExplicitMuxStillClassifiesRtpAndRtcp) {
    auto datagram = std::make_shared<FakeDatagram>();
    auto transport = std::make_shared<UdpMediaTransport>(13, datagram);
    auto sink = std::make_shared<PacketCollector>();
    auto peer = SocketAddr::FromIPPort("127.0.0.1", 5000);
    transport->SetSelectedPeer(peer);
    transport->SetPacketSink(sink);
    ASSERT_TRUE(transport->Start());
    EXPECT_TRUE(transport->IsRtcpMux());
    datagram->Deliver(peer, kRtp);
    datagram->Deliver(peer, kRtcp);
    datagram->Deliver(peer, std::vector<uint8_t>(20));
    ASSERT_EQ(sink->packets.size(), 2U);
    EXPECT_EQ(sink->packets[0].type, MediaPacketType::Rtp);
    EXPECT_EQ(sink->packets[1].type, MediaPacketType::Rtcp);
}

TEST(UdpBinding, RejectsWrongPeerMalformedPacketsAndPacketsAfterClose) {
    auto rtp = std::make_shared<FakeDatagram>();
    auto rtcp = std::make_shared<FakeDatagram>();
    auto transport = std::make_shared<UdpMediaTransport>(14, rtp, rtcp);
    auto sink = std::make_shared<PacketCollector>();
    auto peer = SocketAddr::FromIPPort("127.0.0.1", 5000);
    transport->SetSelectedPeer(peer);
    transport->SetPacketSink(sink);
    ASSERT_TRUE(transport->Start());
    rtp->Deliver(SocketAddr::FromIPPort("127.0.0.2", 5000), kRtp);
    rtp->Deliver(peer, kRtcp); // Too short for the role bound to the RTP socket.
    auto bad = kRtcp;
    bad[3] = 255;
    rtcp->Deliver(peer, bad);
    transport->Close();
    rtp->Deliver(peer, kRtp);
    EXPECT_TRUE(sink->packets.empty());
    EXPECT_EQ(transport->SendRtcp(kRtcp.data(), kRtcp.size()), SendResult::Closed);
}

struct UdpClient {
    network::UdpSocket socket;
    UdpClient() {
        if (socket.Create() < 0 || !socket.Bind("127.0.0.1", 0, false)) std::abort();
    }
    ~UdpClient() { socket.Close(); }
    SocketAddr Address() const {
        sockaddr_storage address{};
        socklen_t size = sizeof(address);
        ::getsockname(socket.Fd(), reinterpret_cast<sockaddr*>(&address), &size);
        return SocketAddr::FromSockaddr(reinterpret_cast<sockaddr*>(&address), size);
    }
    std::vector<uint8_t> Receive(SocketAddr& source) {
        pollfd fd{socket.Fd(), POLLIN, 0};
        if (::poll(&fd, 1, 2000) <= 0) return {};
        std::vector<uint8_t> bytes(65536);
        int size = socket.RecvFrom(bytes.data(), bytes.size(), source);
        if (size < 0) return {};
        bytes.resize(size);
        return bytes;
    }
};

TEST(UdpBinding, RealPairLearnsNatPortsAndReleasesBothSockets) {
    EventLoop loop;
    ASSERT_TRUE(loop.Start());
    UdpClient rtp_client, rtcp_client;
    auto transport = UdpMediaTransport::CreateUnicast(15, loop.GetTaskScheduler(),
        "127.0.0.1", "127.0.0.1", 9, 10);
    ASSERT_NE(transport, nullptr);
    auto rtp_port = transport->LocalPort(MediaPacketType::Rtp);
    auto rtcp_port = transport->LocalPort(MediaPacketType::Rtcp);
    EXPECT_EQ(rtp_port % 2, 0);
    EXPECT_EQ(rtcp_port, rtp_port + 1);
    network::UdpSocket occupied;
    ASSERT_GE(occupied.Create(), 0);
    EXPECT_FALSE(occupied.Bind("127.0.0.1", rtp_port, false));
    occupied.Close();
    auto sink = std::make_shared<PacketCollector>();
    transport->SetPacketSink(sink);
    EXPECT_EQ(rtp_client.socket.SendTo(SocketAddr::FromIPPort("127.0.0.1", rtp_port), kRtp.data(), kRtp.size()), 0);
    EXPECT_EQ(rtcp_client.socket.SendTo(SocketAddr::FromIPPort("127.0.0.1", rtcp_port), kRtcp.data(), kRtcp.size()), 0);
    ASSERT_TRUE(sink->Wait(2));
    EXPECT_TRUE(transport->IsSelectedPeer(MediaPacketType::Rtp, rtp_client.Address()));
    EXPECT_TRUE(transport->IsSelectedPeer(MediaPacketType::Rtcp, rtcp_client.Address()));
    EXPECT_EQ(transport->InputDatagram(SocketAddr::FromIPPort("127.0.0.1", 99), MediaPacketType::Rtcp,
        kRtcp.data(), kRtcp.size(), 1), MediaPacketIngressResult::Dropped);
    ASSERT_EQ(transport->SendRtcp(kRtcp.data(), kRtcp.size()), SendResult::Ok);
    SocketAddr source;
    EXPECT_EQ(rtcp_client.Receive(source), kRtcp);
    EXPECT_EQ(source.Port(), rtcp_port);
    transport->Close();
    network::UdpSocket probe;
    ASSERT_GE(probe.Create(), 0);
    EXPECT_TRUE(probe.Bind("127.0.0.1", rtp_port, false));
    probe.Close();
    ASSERT_GE(probe.Create(), 0);
    EXPECT_TRUE(probe.Bind("127.0.0.1", rtcp_port, false));
    probe.Close();
    loop.Stop();
}

TEST(RtspUdpSetup, RejectsInvalidPortsAndInterleavedNumbers) {
    for (const auto* value : {"RTP/AVP;client_port=0-1", "RTP/AVP;client_port=5000-70000",
         "RTP/AVP;client_port=abc-5001", "RTP/AVP;client_port=5000-5001junk",
         "RTP/AVP;client_port=5000-5000", "RTP/AVP/TCP;interleaved=0-256",
         "RTP/AVP/TCP;interleaved=1-1", "RTP/AVP/TCP;interleaved=-1-2"}) {
        RtspTransport transport;
        EXPECT_FALSE(rtsp::RtspUtil::ParseTransport(value, transport)) << value;
    }
    RtspTransport transport;
    EXPECT_TRUE(rtsp::RtspUtil::ParseTransport("RTP/AVP/UDP;unicast;client_port=5000-5001", transport));
    EXPECT_EQ(transport.lower_transport, "UDP");
    EXPECT_EQ(transport.client_rtp_port, 5000);
    EXPECT_TRUE(rtsp::RtspUtil::ParseTransport("RTP/AVP/TCP;unicast;interleaved=0-1", transport));
    EXPECT_EQ(transport.client_rtp_port, -1); // Parsing must reset previous state.
}

class FrameCollector : public media::IEncodedFramePublisher {
public:
    size_t Publish(const media::EncodedFrameEvent& event) override {
        if (!seen.exchange(true)) frame.set_value(event.frame);
        return 1;
    }
    std::atomic<bool> seen{false};
    std::promise<media::EncodedFrame::ConstPtr> frame;
};

class RtspUdpIntegration : public testing::Test {
protected:
    void SetUp() override {
        registry.Add(std::make_shared<MediaWorkerModule>());
        ASSERT_EQ(registry.RegisterAll(), 0);
        ASSERT_TRUE(loop.Start());
        server = std::make_shared<RtspServer>(&loop);
        ASSERT_TRUE(server->Start("127.0.0.1", 0));
        client = ::socket(AF_INET, SOCK_STREAM, 0);
        ASSERT_GE(client, 0);
        auto address = SocketAddr::FromIPPort("127.0.0.1", server->GetPort());
        ASSERT_EQ(::connect(client, reinterpret_cast<sockaddr*>(&address.ss), address.len), 0);
        timeval timeout{2, 0};
        ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    }
    void TearDown() override {
        if (client >= 0) ::close(client);
        if (server) server->Stop();
        registry.UnregisterAll();
        auto session = MediaSessionManager::Instance().GetSessionBySuffix("live/udp_binding");
        if (session) MediaSessionManager::Instance().RemoveSession(session->GetId());
        server.reset();
        loop.Stop();
    }
    std::string Request(const std::string& method, const std::string& suffix,
                        const std::string& headers = {}, const std::string& body = {}) {
        const auto request = method + " rtsp://127.0.0.1/live/udp_binding" + suffix +
            " RTSP/1.0\r\nCSeq: " + std::to_string(++cseq) + "\r\n" + headers +
            "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
        EXPECT_EQ(::send(client, request.data(), request.size(), MSG_NOSIGNAL), request.size());
        std::string response;
        char byte;
        while (response.find("\r\n\r\n") == std::string::npos && ::recv(client, &byte, 1, 0) == 1)
            response.push_back(byte);
        return response;
    }
    void Announce() {
        auto response = Request("ANNOUNCE", "", "Content-Type: application/sdp\r\n",
            "v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=test\r\nt=0 0\r\n"
            "m=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\na=control:trackID=0\r\n");
        ASSERT_NE(response.find("200 OK"), std::string::npos) << response;
    }
    EventLoop loop;
    WorkerModuleRegistry registry;
    std::shared_ptr<RtspServer> server;
    int client = -1, cseq = 0;
};

TEST_F(RtspUdpIntegration, SetupReceiveFrameRtcpFeedbackAndTeardown) {
    Announce();
    auto session = MediaSessionManager::Instance().GetSessionBySuffix("live/udp_binding");
    ASSERT_NE(session, nullptr);
    auto collector = std::make_shared<FrameCollector>();
    auto frame = collector->frame.get_future();
    session->SetFramePublisher(collector);
    UdpClient rtp_client, rtcp_client;
    const auto headers = "Transport: RTP/AVP;unicast;client_port=" +
        std::to_string(rtp_client.Address().Port()) + "-" + std::to_string(rtcp_client.Address().Port()) + ";mode=record\r\n";
    auto response = Request("SETUP", "/trackID=0", headers);
    ASSERT_NE(response.find("200 OK"), std::string::npos) << response;
    auto begin = response.find("Transport: ");
    ASSERT_NE(begin, std::string::npos);
    begin += 11;
    RtspTransport transport;
    ASSERT_TRUE(rtsp::RtspUtil::ParseTransport(response.substr(begin, response.find("\r\n", begin) - begin), transport));
    EXPECT_GT(transport.server_rtp_port, 0);
    EXPECT_EQ(transport.server_rtcp_port, transport.server_rtp_port + 1);
    EXPECT_EQ(transport.interleaved_rtp, -1);
    const auto endpoint_id = session->FindEndpointByTrack(0);
    ASSERT_NE(endpoint_id, 0U);
    const auto session_header = "Session: " + std::to_string(session->GetId()) + "\r\n";
    EXPECT_NE(Request("RECORD", "", session_header).find("200 OK"), std::string::npos);
    ASSERT_EQ(rtp_client.socket.SendTo(SocketAddr::FromIPPort("127.0.0.1", transport.server_rtp_port),
        kRtp.data(), kRtp.size()), 0);
    ASSERT_EQ(frame.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(*frame.get()->buffer, (std::vector<uint8_t>{0, 0, 0, 1, 0x65, 0xAA, 0xBB}));
    // Sender Report on the RTCP port must reach the receiver statistics path
    // and produce a Receiver Report from that same RTCP port.
    std::vector<uint8_t> sr{0x80, 200, 0, 6, 0x11, 0x22, 0x33, 0x44,
        0xE9, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0x5F, 0x90, 0, 0, 0, 1, 0, 0, 0, 3};
    ASSERT_EQ(rtcp_client.socket.SendTo(SocketAddr::FromIPPort("127.0.0.1", transport.server_rtcp_port), sr.data(), sr.size()), 0);
    SocketAddr source;
    auto feedback = rtcp_client.Receive(source);
    ASSERT_GE(feedback.size(), 8U);
    EXPECT_EQ(feedback[1], 201);
    EXPECT_EQ(source.Port(), transport.server_rtcp_port);
    EXPECT_NE(Request("TEARDOWN", "", session_header).find("200 OK"), std::string::npos);
    EXPECT_FALSE(utils::EndpointManager::Instance().Exists(endpoint_id));
    EXPECT_EQ(session->FindEndpointByTrack(0), 0U);
    // The same track can be set up again after its binding was released.
    EXPECT_NE(Request("SETUP", "/trackID=0", headers).find("200 OK"), std::string::npos);
}

TEST_F(RtspUdpIntegration, InvalidSetupDoesNotConsumeTrackAndTcpStillWorks) {
    Announce();
    EXPECT_NE(Request("SETUP", "/trackID=0", "Transport: RTP/AVP;unicast\r\n").find("461"), std::string::npos);
    EXPECT_NE(Request("SETUP", "/trackID=0", "Transport: RTP/AVP;client_port=5000-5001;rtcp-mux\r\n").find("461"), std::string::npos);
    EXPECT_NE(Request("SETUP", "/trackID=0", "Transport: RTP/AVP/TCP;unicast;interleaved=4-5\r\n").find("interleaved=4-5"), std::string::npos);
}

TEST_F(RtspUdpIntegration, FailedAllocationDoesNotRegisterEndpoint) {
    Announce();
    auto session = MediaSessionManager::Instance().GetSessionBySuffix("live/udp_binding");
    ASSERT_NE(session, nullptr);
    auto before = utils::EndpointManager::Instance().Size();
    RtspRequest request;
    RtspRequest::RtspRequestInfo setup;
    setup.cseq = 99;
    setup.url = "rtsp://127.0.0.1/live/udp_binding/trackID=0";
    setup.headers["transport"] = "RTP/AVP;unicast;client_port=5000-5001";
    auto response = request.HandleCmdSetup(setup, [](uint64_t, RtspTransport&) { return false; });
    EXPECT_NE(response.find("461"), std::string::npos);
    EXPECT_EQ(request.GetLastSetupEndpointId(), 0U);
    EXPECT_EQ(session->FindEndpointByTrack(0), 0U);
    EXPECT_EQ(utils::EndpointManager::Instance().Size(), before);
}

TEST(UdpBinding, TransportMayBeReleasedInsideReceiveCallback) {
    class ReleasingSink : public IMediaPacketSink {
    public:
        std::shared_ptr<UdpMediaTransport> transport;
        std::promise<void> done;
        MediaPacketIngressResult OnMediaPacket(ReceivedMediaPacket) override {
            transport.reset();
            done.set_value();
            return MediaPacketIngressResult::Accepted;
        }
    };
    EventLoop loop;
    ASSERT_TRUE(loop.Start());
    UdpClient client;
    auto sink = std::make_shared<ReleasingSink>();
    sink->transport = UdpMediaTransport::CreateUnicast(16, loop.GetTaskScheduler(),
        "127.0.0.1", "127.0.0.1", 9, 10);
    ASSERT_NE(sink->transport, nullptr);
    const auto port = sink->transport->LocalPort(MediaPacketType::Rtp);
    sink->transport->SetPacketSink(sink);
    auto done = sink->done.get_future();
    ASSERT_EQ(client.socket.SendTo(SocketAddr::FromIPPort("127.0.0.1", port), kRtp.data(), kRtp.size()), 0);
    ASSERT_EQ(done.wait_for(2s), std::future_status::ready);
    loop.Stop();
    EXPECT_EQ(sink->transport, nullptr);
}
}
