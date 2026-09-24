#include "WebRtcSession.h"

#include <iostream>
#include <stdexcept>
#include <utility>

using namespace protocol::webrtc;
using Protocol = network::transport::DatagramProtocol;
using SendResult = network::transport::DatagramSendResult;

#define CHECK(value) do { if (!(value)) throw std::runtime_error(#value); } while (false)

namespace
{
class Datagram final : public network::transport::IDatagramTransport
{
public:
    uint64_t Id() const noexcept override { return 10; }
    bool IsWritable() const noexcept override { return writable; }
    SendResult SendDatagram(const network::SocketAddr&, const uint8_t* data, size_t size) override
    {
        last.assign(data, data + size);
        ++sent;
        return writable ? SendResult::Ok : SendResult::Closed;
    }
    void SetDatagramSink(std::weak_ptr<network::transport::IDatagramSink> value) override { sink = value; }
    void Close() override { writable = false; }
    bool writable = true;
    int sent = 0;
    std::vector<uint8_t> last;
    std::weak_ptr<network::transport::IDatagramSink> sink;
};

class Dtls final : public DtlsTransport
{
public:
    DtlsParameters LocalParameters() const override
    {
        DtlsParameters result;
        result.fingerprints.push_back({"sha-256", "LOCAL-FINGERPRINT"});
        return result;
    }
    bool Configure(const DtlsParameters& remote, DtlsSetup role) override
    {
        configuredRemote = remote;
        configuredRole = role;
        return configureOk;
    }
    bool Start(SendCallback callback) override { ++starts; send = std::move(callback); return startOk; }
    bool HandleDatagram(const uint8_t*, size_t) override { connected = handshakeOk; return handshakeOk; }
    bool Tick(uint64_t) override { return tickOk; }
    bool IsConnected() const noexcept override { return connected; }
    bool ExportSrtpKeys(SrtpKeyingMaterial& keys) const override
    {
        keys.profile = 1;
        keys.sendKey.assign(30, 1);
        keys.receiveKey.assign(30, 2);
        return exportOk;
    }
    void Close() noexcept override { connected = false; send = {}; ++closes; }
    bool connected = false, configureOk = true, startOk = true;
    bool handshakeOk = true, tickOk = true, exportOk = true;
    int starts = 0, closes = 0;
    DtlsParameters configuredRemote;
    DtlsSetup configuredRole = DtlsSetup::Unspecified;
    SendCallback send;
};

// Identity transformation is ONLY a test double, never a crypto implementation.
class Srtp final : public SrtpTransport
{
public:
    bool Configure(const SrtpKeyingMaterial& keys) override
    {
        CHECK(keys.sendKey != keys.receiveKey);
        ++installs;
        return configureOk;
    }
    bool UnprotectRtp(std::vector<uint8_t>&) override { ++decrypts; return authenticate; }
    bool UnprotectRtcp(std::vector<uint8_t>&) override { ++decrypts; return authenticate; }
    bool ProtectRtp(std::vector<uint8_t>& packet) override { packet.push_back(0xaa); return true; }
    bool ProtectRtcp(std::vector<uint8_t>& packet) override { packet.push_back(0xbb); return true; }
    void Close() noexcept override { ++closes; }
    int installs = 0, decrypts = 0, closes = 0;
    bool authenticate = true, configureOk = true;
};

class Sink final : public IMediaPacketSink
{
public:
    MediaPacketIngressResult OnMediaPacket(ReceivedMediaPacket packet) override
    {
        packets.push_back(std::move(packet));
        return MediaPacketIngressResult::Accepted;
    }
    std::vector<ReceivedMediaPacket> packets;
};

WebRtcMediaDescription Audio()
{
    WebRtcMediaDescription media;
    media.mid = "audio";
    media.media = "audio";
    media.port = 9;
    media.proto = "UDP/TLS/RTP/SAVPF";
    media.fmts = {"111"};
    media.rtcpMux = true;
    media.codecs.push_back({111, "opus", 48000, 2, "minptime=10", {{"nack", ""}}});
    return media;
}

WebRtcSessionDescription Offer()
{
    WebRtcSessionDescription offer;
    offer.ice.ufrag = "remote";
    offer.ice.pwd = std::string(24, 'r');
    offer.dtls.setup = DtlsSetup::ActPass;
    offer.dtls.fingerprints.push_back({"sha-256", "REMOTE-FINGERPRINT"});
    offer.bundle.mids = {"audio"};
    offer.medias.push_back(Audio());
    return offer;
}

WebRtcSessionOptions Options()
{
    WebRtcSessionOptions options;
    options.ice.ufrag = "local";
    options.ice.pwd = std::string(24, 'l');
    options.ice.candidates = {"1 1 udp 2130706431 192.0.2.1 5000 typ host"};
    options.ice.endOfCandidates = true;
    options.medias.push_back(Audio());
    return options;
}

struct Fixture
{
    std::shared_ptr<Datagram> udp = std::make_shared<Datagram>();
    std::shared_ptr<WebRtcTransport> transport = std::make_shared<WebRtcTransport>(20, udp);
    std::shared_ptr<Sink> sink = std::make_shared<Sink>();
    Dtls* dtls = nullptr;
    Srtp* srtp = nullptr;
    std::shared_ptr<WebRtcSession> session;
    network::SocketAddr peer = network::SocketAddr::FromIPPort("192.0.2.2", 6000);

    explicit Fixture(WebRtcSessionOptions options = Options())
    {
        auto d = std::make_unique<Dtls>();
        auto s = std::make_unique<Srtp>();
        dtls = d.get(); srtp = s.get();
        session = std::make_shared<WebRtcSession>(transport, std::move(d), std::move(s), sink, std::move(options));
    }
    void Start(WebRtcSessionDescription offer = Offer())
    {
        WebRtcSessionDescription answer;
        CHECK(session->ApplyRemoteOffer(offer));
        CHECK(session->CreateLocalAnswer(answer));
        CHECK(session->start());
    }
    void Deliver(std::vector<uint8_t> data, const network::SocketAddr& from)
    {
        transport->OnDatagram({10, 1234, from, data.data(), data.size()});
    }
    void Handshake()
    {
        // Model ICE's selected-peer output at the transport boundary. Real
        // authenticated STUN nomination is covered by the ICE integration test.
        CHECK(transport->SelectPeer(peer));
        std::vector<uint8_t> record(13);
        record[0] = 22; record[1] = 0xfe; record[2] = 0xfd;
        Deliver(record, peer);
    }
};

std::vector<uint8_t> Rtp()
{
    std::vector<uint8_t> packet(12);
    packet[0] = 0x80; packet[1] = 111;
    return packet;
}

std::vector<uint8_t> Rtcp() { return {0x80, 201, 0, 1, 0, 0, 0, 1}; }

void Negotiation()
{
    Fixture f;
    WebRtcSessionDescription answer;
    CHECK(!f.session->start());
    CHECK(!f.session->CreateLocalAnswer(answer));
    auto offer = Offer();
    offer.medias[0].direction = MediaDirection::SendOnly;
    offer.medias[0].attributes.push_back({"ssrc", "123 cname:REMOTE"});
    WebRtcMediaDescription data;
    data.mid = "data"; data.media = "application";
    data.port = 9; data.proto = "UDP/DTLS/SCTP"; data.fmts = {"webrtc-datachannel"};
    offer.medias.push_back(data);
    offer.bundle.mids.push_back("data");
    CHECK(f.session->ApplyRemoteOffer(offer));
    auto invalid = offer;
    invalid.medias[1].mid = "audio";
    CHECK(!f.session->ApplyRemoteOffer(invalid));
    CHECK(f.session->State() == WebRtcSessionState::HaveOffer);
    CHECK(f.session->CreateLocalAnswer(answer));
    CHECK(answer.type == SdpType::Answer && answer.ice.iceLite);
    CHECK(answer.medias.size() == 2);
    CHECK(answer.medias[0].direction == MediaDirection::RecvOnly);
    CHECK(answer.medias[0].dtls.setup == DtlsSetup::Active);
    CHECK(answer.medias[0].ice.ufrag == "local");
    CHECK(answer.medias[0].dtls.fingerprints[0].value == "LOCAL-FINGERPRINT");
    CHECK(answer.medias[0].ice.pwd == std::string(24, 'l'));
    CHECK(answer.medias[0].ssrcs.empty());
    CHECK(answer.medias[1].port == 0);
    CHECK(answer.bundle.mids == std::vector<std::string>{"audio"});
    const auto id = answer.origin.sess_id;
    CHECK(f.session->CreateLocalAnswer(answer) && answer.origin.sess_id == id);
    CHECK(f.session->start() && f.session->start());
    CHECK(f.dtls->configuredRemote.fingerprints[0].value == "REMOTE-FINGERPRINT");
    CHECK(!f.session->ApplyRemoteOffer(offer));
    f.Handshake();
    CHECK(!f.session->SendRtp(Rtp())); // recvonly still permits RTCP.
    CHECK(f.session->SendRtcp(Rtcp()));
}

void Validation()
{
    for (int kind = 0; kind < 7; ++kind)
    {
        Fixture f;
        auto offer = Offer();
        switch (kind)
        {
        case 0: offer.type = SdpType::Answer; break;
        case 1: offer.ice.pwd.clear(); break;
        case 2: offer.dtls.fingerprints.clear(); break;
        case 3: offer.bundle.mids = {"missing"}; break;
        case 4: offer.medias[0].rtcpMux = false; break;
        case 5: offer.ice.iceLite = true; break;
        case 6: offer.medias[0].codecs[0].payloadType = 72; break;
        }
        CHECK(!f.session->ApplyRemoteOffer(offer));
        CHECK(f.session->State() == WebRtcSessionState::New);
        CHECK(!f.session->LastError().empty());
    }
    Fixture f;
    auto offer = Offer();
    offer.medias[0].codecs[0].encodingName = "unknown";
    CHECK(f.session->ApplyRemoteOffer(offer));
    WebRtcSessionDescription answer;
    answer.ice.ufrag = "sentinel";
    CHECK(!f.session->CreateLocalAnswer(answer));
    CHECK(answer.ice.ufrag == "sentinel");
    auto options = Options();
    options.ice.candidates.clear();
    Fixture missing(options);
    CHECK(missing.session->ApplyRemoteOffer(Offer()));
    CHECK(!missing.session->CreateLocalAnswer(answer));
}

void BundleAndDirections()
{
    auto options = Options();
    auto video = Audio();
    video.mid = "video";
    video.media = "video";
    video.fmts = {"96"};
    video.codecs = {{96, "VP8", 90000, 0, "", {}}};
    options.medias.push_back(video);
    Fixture f(options);
    auto offer = Offer();
    video.port = 0;
    video.bundleOnly = true;
    video.direction = MediaDirection::RecvOnly;
    offer.medias.push_back(video);
    offer.bundle.mids.push_back("video");
    offer.medias[0].direction = MediaDirection::Inactive;
    offer.dtls.setup = DtlsSetup::Active;
    CHECK(f.session->ApplyRemoteOffer(offer));
    WebRtcSessionDescription answer;
    CHECK(f.session->CreateLocalAnswer(answer));
    CHECK(answer.medias[0].direction == MediaDirection::Inactive);
    CHECK(answer.medias[1].port != 0 && !answer.medias[1].bundleOnly);
    CHECK(answer.medias[1].direction == MediaDirection::SendOnly);
    CHECK(answer.dtls.setup == DtlsSetup::Passive);
    CHECK(answer.bundle.mids.size() == 2);
    CHECK(f.session->start());
    f.Handshake();
    f.Deliver(Rtp(), f.peer);
    CHECK(f.sink->packets.empty());
    CHECK(!f.session->SendRtp(Rtp()));
    CHECK(f.session->SendRtcp(Rtcp()));
}

void MediaFlow()
{
    Fixture f;
    f.Start();
    CHECK(f.session->State() == WebRtcSessionState::Connecting);
    CHECK(f.dtls->starts == 0 && !f.session->SendRtp(Rtp()));
    f.Deliver(Rtp(), f.peer);
    CHECK(f.srtp->decrypts == 0 && f.sink->packets.empty());
    CHECK(f.transport->SelectPeer(f.peer));
    f.Deliver(Rtp(), f.peer);
    CHECK(f.srtp->decrypts == 0);
    f.Handshake();
    CHECK(f.session->State() == WebRtcSessionState::Connected);
    CHECK(f.dtls->starts == 1 && f.srtp->installs == 1);
    f.Handshake();
    CHECK(f.srtp->installs == 1);
    f.Deliver(Rtp(), network::SocketAddr::FromIPPort("192.0.2.3", 6000));
    CHECK(f.srtp->decrypts == 0);
    f.srtp->authenticate = false;
    f.Deliver(Rtp(), f.peer); f.Deliver(Rtcp(), f.peer);
    CHECK(f.sink->packets.empty());
    f.srtp->authenticate = true;
    auto unknown = Rtp(); unknown[1] = 112;
    f.Deliver(unknown, f.peer);
    CHECK(f.sink->packets.empty());
    f.Deliver(Rtp(), f.peer); f.Deliver(Rtcp(), f.peer);
    CHECK(f.sink->packets.size() == 2);
    CHECK(f.sink->packets[0].payload.ToVector() == Rtp());
    CHECK(f.sink->packets[0].receive_time_ms == 1234);
    CHECK(f.sink->packets[1].type == MediaPacketType::Rtcp);
    CHECK(f.session->SendRtp(Rtp()) && f.udp->last.back() == 0xaa);
    CHECK(f.session->SendRtcp(Rtcp()) && f.udp->last.back() == 0xbb);
    CHECK(f.session->stop() && f.session->stop());
    CHECK(f.udp->sink.expired());
    CHECK(!f.session->SendRtcp(Rtcp()) && !f.session->start());
    f.Deliver(Rtp(), f.peer);
    CHECK(f.sink->packets.size() == 2);
}

void Failures()
{
    for (int kind = 0; kind < 5; ++kind)
    {
        Fixture f;
        f.Start();
        if (kind == 0) f.dtls->handshakeOk = false;
        if (kind == 1) f.dtls->exportOk = false;
        if (kind == 2) f.srtp->configureOk = false;
        if (kind == 3) f.dtls->startOk = false;
        f.Handshake();
        if (kind == 4) { f.dtls->tickOk = false; CHECK(!f.session->Tick(100)); }
        CHECK(f.session->State() == WebRtcSessionState::Failed);
        CHECK(f.udp->sink.expired() && f.transport->State() == WebRtcTransportState::Closed);
        CHECK(f.dtls->closes > 0 && f.srtp->closes > 0);
        CHECK(!f.session->SendRtp(Rtp()));
    }
    Fixture closed;
    closed.Start();
    closed.transport->Close();
    CHECK(closed.session->State() == WebRtcSessionState::Closed);
    Fixture failedStart;
    WebRtcSessionDescription answer;
    CHECK(failedStart.session->ApplyRemoteOffer(Offer()));
    CHECK(failedStart.session->CreateLocalAnswer(answer));
    failedStart.udp->writable = false;
    CHECK(!failedStart.session->start());
    CHECK(failedStart.session->State() == WebRtcSessionState::Failed);
}

void IceTimeout()
{
    for (int action = 0; action < 4; ++action)
    {
        uint64_t now = 0;
        auto options = Options();
        options.iceTimeoutMs = 100;
        options.iceClock = [&] { return now; };
        Fixture f(options);
        f.Start();
        if (action != 0) f.Handshake();
        now = 99;
        CHECK(f.session->Tick(99));
        now = 100;
        if (action == 0) CHECK(!f.session->Tick(100));
        if (action == 1) CHECK(!f.session->SendRtp(Rtp()));
        if (action == 2) CHECK(!f.session->SendRtcp(Rtcp()));
        if (action == 3) f.Deliver(Rtp(), f.peer);
        CHECK(f.session->State() == WebRtcSessionState::Failed);
        CHECK(f.session->LastError() == "ICE connectivity check timeout");
        CHECK(f.transport->State() == WebRtcTransportState::Closed);
        CHECK(f.udp->sink.expired());
        CHECK(f.dtls->closes > 0 && f.srtp->closes > 0);
        CHECK(f.sink->packets.empty());
        CHECK(!f.session->SendRtp(Rtp()) && !f.session->SendRtcp(Rtcp()));
        CHECK(!f.session->Tick(101));
    }
}

void StunNomination()
{
    uint64_t now = 0;
    auto options = Options();
    options.iceTimeoutMs = 100;
    options.iceClock = [&] { return now; };
    Fixture f(options);
    f.Start();
    // Even before peer selection, malformed/unauthenticated STUN is routed to
    // ICE and cannot open the DTLS/media path.
    std::vector<uint8_t> malformed(20);
    malformed[1] = 1;
    malformed[4] = 0x21; malformed[5] = 0x12; malformed[6] = 0xa4; malformed[7] = 0x42;
    f.Deliver(malformed, f.peer);
    CHECK(!f.transport->IsSelectedPeer(f.peer) && f.dtls->starts == 0);

#if __has_include(<openssl/hmac.h>)
    protocol::IceRequestParams request;
    request.username = "local:remote";
    request.password = std::string(24, 'l');
    request.controlling = true;
    request.tie_breaker = 42;
    request.priority = 1234;
    request.use_candidate = true;
    std::vector<uint8_t> bytes(1500);
    size_t size = 0;
    CHECK(protocol::StunCodec::BuildIceBindingRequest(request, bytes.data(), bytes.size(), size));
    bytes.resize(size);
    f.Deliver(bytes, f.peer);
    CHECK(f.transport->IsSelectedPeer(f.peer));
    CHECK(f.dtls->starts == 1);
    CHECK(f.session->State() == WebRtcSessionState::Connecting);
    protocol::StunMessageInfo response;
    CHECK(protocol::StunCodec::Parse(f.udp->last.data(), f.udp->last.size(), response));
    CHECK(response.IsBindingResponse());
    CHECK(protocol::StunCodec::VerifyMessageIntegrity(response, request.password));
    f.Handshake();
    now = 90;
    request.use_candidate = false;
    bytes.resize(1500);
    CHECK(protocol::StunCodec::BuildIceBindingRequest(request, bytes.data(), bytes.size(), size));
    bytes.resize(size);
    f.Deliver(bytes, f.peer);
    now = 100;
    CHECK(f.session->Tick(100));
    CHECK(f.session->SendRtp(Rtp()));
    now = 190;
    CHECK(!f.session->Tick(190));
    CHECK(f.session->State() == WebRtcSessionState::Failed);
    f.Deliver(bytes, f.peer);
    CHECK(f.session->State() == WebRtcSessionState::Failed);
#else
    std::cout << "Authenticated STUN nomination skipped: OpenSSL headers unavailable\n";
#endif
}
} // namespace

int main()
{
    try
    {
        Negotiation(); Validation(); BundleAndDirections(); MediaFlow(); Failures(); StunNomination(); IceTimeout();
        std::cout << "WebRtcSession negotiation, validation, media and failure tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "WebRtcSession test failed: " << error.what() << '\n';
        return 1;
    }
}
