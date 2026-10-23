#include <gtest/gtest.h>
#include "WebRtcMediaTransport.h"
#include <future>

namespace
{
using namespace protocol::webrtc;
class AdapterDatagram : public network::transport::IDatagramTransport
{
public:
    uint64_t Id() const noexcept override { return 10; }
    bool IsWritable() const noexcept override { return !closed; }
    network::transport::DatagramSendResult SendDatagram(const network::SocketAddr&, const uint8_t*, size_t) override {
        ++sends;
        return network::transport::DatagramSendResult::Ok;
    }
    void SetDatagramSink(std::weak_ptr<network::transport::IDatagramSink>) override {}
    void Close() override { closed = true; }
    bool closed = false;
    size_t sends = 0;
};

// Test-only DTLS/SRTP doubles. These do not provide authentication or encryption.
class AdapterDtls : public DtlsTransport
{
public:
    DtlsParameters LocalParameters() const override {
        DtlsParameters value;
        value.fingerprints.push_back({"sha-256", "LOCAL"});
        return value;
    }
    bool Configure(const DtlsParameters&, DtlsSetup) override { return true; }
    bool Start(SendCallback) override { return true; }
    bool HandleDatagram(const uint8_t*, size_t) override { connected = true; return true; }
    bool Tick(uint64_t) override { return true; }
    bool IsConnected() const noexcept override { return connected; }
    bool ExportSrtpKeys(SrtpKeyingMaterial& keys) const override {
        keys.profile = 1; keys.sendKey = {1}; keys.receiveKey = {2}; return true;
    }
    void Close() noexcept override { connected = false; }
    bool connected = false;
};
class AdapterSrtp : public SrtpTransport
{
public:
    bool Configure(const SrtpKeyingMaterial&) override { return true; }
    bool UnprotectRtp(std::vector<uint8_t>&) override { return true; }
    bool UnprotectRtcp(std::vector<uint8_t>&) override { return true; }
    bool ProtectRtp(std::vector<uint8_t>&) override { ++protects; return true; }
    bool ProtectRtcp(std::vector<uint8_t>&) override { ++protects; return true; }
    void Close() noexcept override {}
    size_t protects = 0;
};
class AdapterSink : public IMediaPacketSink
{
public:
    MediaPacketIngressResult OnMediaPacket(ReceivedMediaPacket packet) override {
        last = std::move(packet); ++packets; return MediaPacketIngressResult::Accepted;
    }
    ReceivedMediaPacket last;
    size_t packets = 0;
};

TEST(WebRtcMediaAdapter, ReceivesPlaintextAndQueuesEncryptionOnSessionLoop)
{
    std::vector<std::function<void()>> tasks;
    auto adapter = std::make_shared<media::transport::WebRtcMediaTransport>(20,
        [&](auto task) { tasks.push_back(std::move(task)); return true; });
    auto udp = std::make_shared<AdapterDatagram>();
    auto wire = std::make_shared<WebRtcTransport>(20, udp);
    auto crypto = std::make_unique<AdapterSrtp>();
    auto* counters = crypto.get();
    WebRtcMediaDescription audio;
    audio.media = "audio"; audio.mid = "a"; audio.port = 9;
    audio.proto = "UDP/TLS/RTP/SAVPF"; audio.fmts = {"111"}; audio.rtcpMux = true;
    audio.codecs.push_back({111, "opus", 48000, 2, "", {}});
    WebRtcSessionOptions options;
    options.ice.ufrag = "local"; options.ice.pwd = std::string(24, 'l');
    options.ice.candidates = {"1 1 udp 2130706431 192.0.2.1 5000 typ host"};
    options.medias = {audio};
    options.onNetworkPathChanged = [weak = std::weak_ptr<media::transport::WebRtcMediaTransport>(adapter)]
        (const WebRtcNetworkPath& path) {
        if (auto target = weak.lock()) target->OnNetworkPathChanged(path);
    };
    auto session = std::make_shared<WebRtcSession>(wire, std::make_unique<AdapterDtls>(),
        std::move(crypto), adapter, options);
    ASSERT_TRUE(adapter->BindSession(session));
    auto sink = std::make_shared<AdapterSink>();
    adapter->SetPacketSink(sink);
    WebRtcSessionDescription offer;
    offer.ice.ufrag = "remote"; offer.ice.pwd = std::string(24, 'r');
    offer.dtls.setup = DtlsSetup::ActPass;
    offer.dtls.fingerprints.push_back({"sha-256", "REMOTE"});
    offer.bundle.mids = {"a"}; offer.medias = {audio};
    WebRtcSessionDescription answer;
    ASSERT_TRUE(session->ApplyRemoteOffer(offer)) << session->LastError();
    ASSERT_TRUE(session->CreateLocalAnswer(answer)) << session->LastError();
    ASSERT_TRUE(session->start());
    const std::vector<uint8_t> rtp{0x80, 111, 0, 1, 0, 0, 0, 1, 0, 0, 0, 42, 1};
    EXPECT_EQ(adapter->SendRtp(rtp.data(), rtp.size()), SendResult::NotWritable);
    const auto peer = network::SocketAddr::FromIPPort("192.0.2.2", 6000);
    // Supply ICE's selected-peer result; this test exercises the media adapter,
    // while STUN nomination belongs to the existing ICE integration tests.
    ASSERT_TRUE(wire->SelectPeer(peer));
    std::vector<uint8_t> dtls(13);
    dtls[0] = 22; dtls[1] = 0xfe; dtls[2] = 0xfd;
    wire->OnDatagram({10, 100, peer, dtls.data(), dtls.size()});
    ASSERT_EQ(adapter->State(), MediaTransportState::Connected);
    wire->OnDatagram({10, 101, peer, rtp.data(), rtp.size()});
    ASSERT_EQ(sink->packets, 1U);
    EXPECT_EQ(sink->last.transport_id, 20U);
    EXPECT_EQ(sink->last.payload.ToVector(), rtp);
    auto sent = std::async(std::launch::async, [&] { return adapter->SendRtp(rtp.data(), rtp.size()); });
    ASSERT_EQ(sent.get(), SendResult::Ok);
    EXPECT_EQ(counters->protects, 0U);
    ASSERT_EQ(tasks.size(), 1U);
    tasks.front()(); tasks.clear();
    EXPECT_EQ(counters->protects, 1U);
    EXPECT_EQ(udp->sends, 1U);
    auto controller = adapter->GetSendSideController();
    media::WeakNetFeedback feedback;
    feedback.now_ms = 100;
    feedback.loss_rate = 0.1;
    controller->OnReceiverFeedback(feedback);
    ASSERT_TRUE(controller->GetNetworkState().HasUpdates());
    // Queue old-path media, then commit a restart before its owner-loop send.
    ASSERT_EQ(adapter->SendRtp(rtp.data(), rtp.size()), SendResult::Ok);
    offer.origin.sess_version = "2";
    offer.ice.ufrag = "newremote";
    offer.ice.pwd = std::string(24, 'n');
    ASSERT_TRUE(session->RestartIce(offer, answer)) << session->LastError();
    EXPECT_FALSE(controller->GetNetworkState().HasUpdates());
    controller->OnReceiverFeedback(feedback);
    EXPECT_FALSE(controller->GetNetworkState().HasUpdates());
    ASSERT_EQ(adapter->SendRtp(rtp.data(), rtp.size()), SendResult::NotWritable);
    protocol::IceRequestParams check;
    check.username = answer.medias.front().ice.ufrag + ":newremote";
    check.password = answer.medias.front().ice.pwd;
    check.controlling = true; check.tie_breaker = 42; check.priority = 1234; check.use_candidate = true;
    std::vector<uint8_t> stun(1500);
    size_t length = 0;
    ASSERT_TRUE(protocol::StunCodec::BuildIceBindingRequest(check, stun.data(), stun.size(), length));
    const auto newPeer = network::SocketAddr::FromIPPort("192.0.2.3", 7000);
    wire->OnDatagram({10, 200, newPeer, stun.data(), length});
    ASSERT_EQ(adapter->State(), MediaTransportState::Connected);
    for (auto& task : tasks) task();
    tasks.clear();
    EXPECT_EQ(counters->protects, 1U); // Old queued media did not cross into the new path.
    EXPECT_FALSE(controller->GetNetworkState().HasUpdates());
    controller->OnReceiverFeedback(feedback);
    EXPECT_TRUE(controller->GetNetworkState().HasUpdates());
    ASSERT_EQ(adapter->SendRtp(rtp.data(), rtp.size()), SendResult::Ok);
    tasks.front()(); tasks.clear();
    EXPECT_EQ(counters->protects, 2U);
    ASSERT_EQ(adapter->SendRtp(rtp.data(), rtp.size()), SendResult::Ok);
    adapter->Close();
    for (auto& task : tasks) task();
    EXPECT_EQ(counters->protects, 2U); // Queued send is cancelled on close.
    EXPECT_EQ(wire->State(), WebRtcTransportState::Closed);
    EXPECT_FALSE(wire->IsSelectedPeer(peer));
    EXPECT_EQ(session->State(), WebRtcSessionState::Closed);
    EXPECT_EQ(adapter->SendRtp(rtp.data(), rtp.size()), SendResult::Closed);
}
}
