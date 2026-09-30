#include "server/WebRtcService.h"
#include "EventLoop.h"
#include "EndpointBase.h"
#include "Sdp.h"
#include "media/endpoint/MediaEndpoint.h"
#include "third/nlohmann/json.hpp"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace
{
using Json = nlohmann::json;
using namespace std::chrono_literals;

void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

class Socket
{
public:
    explicit Socket(int type) : fd_(::socket(AF_INET, type, 0))
    {
        Require(fd_ >= 0, "socket creation failed");
    }
    ~Socket() { Close(); }
    void Close()
    {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }
    int Get() const { return fd_; }
    bool Bind(uint16_t port)
    {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);
        return ::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    }
    uint16_t Port() const
    {
        sockaddr_in addr{};
        socklen_t length = sizeof(addr);
        Require(::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &length) == 0, "getsockname failed");
        return ntohs(addr.sin_port);
    }
private:
    int fd_;
};

uint16_t AvailablePort(int type)
{
    Socket socket(type);
    Require(socket.Bind(0), "ephemeral bind failed");
    return socket.Port();
}

class Client
{
public:
    explicit Client(uint16_t port) : socket_(SOCK_STREAM)
    {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);
        Require(::connect(socket_.Get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0, "connect failed");
        Write("GET /webrtc HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
              "Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
              "Sec-WebSocket-Protocol: packetia\r\n"
              "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n");
        std::string response;
        while (response.find("\r\n\r\n") == std::string::npos)
        {
            response += Read(1);
            Require(response.size() < 16384, "invalid upgrade response");
        }
        Require(response.find(" 101 ") != std::string::npos, "WebSocket upgrade failed");
    }

    void Close() { socket_.Close(); }

    Json Request(const Json& request) { return RequestText(request.dump()); }

    Json RequestText(const std::string& payload)
    {
        Send(1, payload);
        return Receive();
    }

    Json Receive()
    {
        std::string response;
        for (;;)
        {
            const auto header = Read(2);
            const auto first = static_cast<uint8_t>(header[0]);
            const auto second = static_cast<uint8_t>(header[1]);
            Require((second & 0x80) == 0, "server frame must not be masked");
            uint64_t size = second & 0x7f;
            if (size >= 126)
            {
                const auto extended = Read(size == 126 ? 2 : 8);
                size = 0;
                for (unsigned char byte : extended) size = (size << 8) | byte;
            }
            Require(size <= 1024 * 1024, "server response too large");
            const auto body = Read(static_cast<size_t>(size));
            if ((first & 15) == 9) { Send(10, body); continue; }
            Require((first & 15) == 1 || (first & 15) == 0, "expected text response");
            response += body;
            if (first & 0x80) return Json::parse(response);
        }
    }

private:
    void Send(uint8_t opcode, const std::string& payload)
    {
        std::string frame(1, static_cast<char>(0x80 | opcode));
        if (payload.size() < 126) frame.push_back(static_cast<char>(0x80 | payload.size()));
        else
        {
            const int count = payload.size() <= 65535 ? 2 : 8;
            frame.push_back(static_cast<char>(0x80 | (count == 2 ? 126 : 127)));
            for (int shift = (count - 1) * 8; shift >= 0; shift -= 8)
                frame.push_back(static_cast<char>(uint64_t(payload.size()) >> shift));
        }
        constexpr uint8_t mask[] = {1, 2, 3, 4};
        frame.append(reinterpret_cast<const char*>(mask), sizeof(mask));
        for (size_t i = 0; i < payload.size(); ++i)
            frame.push_back(static_cast<char>(static_cast<uint8_t>(payload[i]) ^ mask[i % 4]));
        Write(frame);
    }

    void Write(const std::string& data)
    {
        size_t offset = 0;
        while (offset != data.size())
        {
            const auto sent = ::send(socket_.Get(), data.data() + offset, data.size() - offset, 0);
            Require(sent > 0, "socket write failed");
            offset += static_cast<size_t>(sent);
        }
    }

    std::string Read(size_t size)
    {
        std::string data(size, '\0');
        size_t offset = 0;
        while (offset != size)
        {
            pollfd event{socket_.Get(), POLLIN, 0};
            Require(::poll(&event, 1, 5000) > 0, "socket read timed out");
            const auto received = ::recv(socket_.Get(), data.data() + offset, size - offset, 0);
            Require(received > 0, "socket closed before response");
            offset += static_cast<size_t>(received);
        }
        return data;
    }

    Socket socket_;
};

void WaitForEndpoints(size_t count)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (utils::EndpointManager::Instance().Size() != count)
    {
        Require(std::chrono::steady_clock::now() < deadline, "endpoint cleanup timed out");
        std::this_thread::sleep_for(5ms);
    }
}

std::string Offer()
{
    return "v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\n"
        "a=group:BUNDLE a v\r\na=ice-ufrag:peer\r\na=ice-pwd:abcdefghijklmnopqrstuvwxyz\r\n"
        "a=fingerprint:sha-256 00:01:02:03:04:05:06:07:08:09:0A:0B:0C:0D:0E:0F:10:11:12:13:14:15:16:17:18:19:1A:1B:1C:1D:1E:1F\r\n"
        "a=setup:actpass\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\nc=IN IP4 0.0.0.0\r\n"
        "a=mid:a\r\na=sendonly\r\na=rtcp-mux\r\na=rtpmap:111 opus/48000/2\r\n"
        "a=extmap:1 urn:ietf:params:rtp-hdrext:sdes:mid\r\na=ssrc:1001 cname:test\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 102 103 104\r\nc=IN IP4 0.0.0.0\r\n"
        "a=mid:v\r\na=sendonly\r\na=rtcp-mux\r\n"
        "a=rtpmap:102 H264/90000\r\na=fmtp:102 packetization-mode=1;profile-level-id=42e01f;level-asymmetry-allowed=1\r\n"
        "a=rtpmap:103 H264/90000\r\na=fmtp:103 packetization-mode=1;profile-level-id=42e01e;level-asymmetry-allowed=1\r\n"
        "a=rtpmap:104 rtx/90000\r\na=fmtp:104 apt=102\r\n"
        "a=extmap:1 urn:ietf:params:rtp-hdrext:sdes:mid\r\n"
        "a=ssrc-group:FID 2001 2002\r\na=ssrc:2001 cname:test\r\na=ssrc:2002 cname:test\r\n";
}

void CheckAnswer(const Json& reply, uint16_t port)
{
    Require(reply.value("type", "") == "answer", reply.dump().c_str());
    sdp::SdpSession answer;
    std::string error;
    Require(sdp::Sdp::Parse(reply.at("sdp").get<std::string>(), sdp::SdpProfile::WebRtc,
        sdp::SdpType::Answer, answer, error), "could not parse SDP answer");
    Require(answer.medias.size() == 2 && answer.ice.iceLite, "expected bundled ICE-lite answer");
    for (const auto& media : answer.medias)
    {
        Require(media.direction == sdp::MediaDirection::RecvOnly, "answer must receive media");
        Require(media.codecs.size() == 1, "receiver must negotiate one primary payload type");
        Require(media.codecs.front().rtcpFeedback.empty(), "answer advertised unsupported feedback");
        Require(media.ice.candidates.size() == 1, "answer must advertise shared UDP port");
        Require(media.ice.candidates.front().find("127.0.0.1 " + std::to_string(port) + " typ host") !=
            std::string::npos, "answer UDP address does not match listener");
    }
    Require(answer.medias[1].codecs.front().payloadType == 102, "offer codec preference was not preserved");
}

sdp::SdpSession ParseDescription(const Json& message, sdp::SdpType type)
{
    sdp::SdpSession description;
    std::string error;
    Require(sdp::Sdp::Parse(message.at("sdp").get<std::string>(), sdp::SdpProfile::WebRtc,
        type, description, error), "could not parse negotiated SDP");
    return description;
}

sdp::SdpSession BrowserAnswer(const sdp::SdpSession& offer, const sdp::SdpSession& browser, uint64_t version)
{
    auto answer = offer;
    answer.type = sdp::SdpType::Answer;
    answer.origin = browser.origin;
    answer.origin.sess_version = std::to_string(version);
    answer.ice = browser.ice;
    answer.ice.iceLite = false;
    answer.ice.candidates = {"1 1 UDP 2130706431 127.0.0.1 50000 typ host"};
    answer.ice.endOfCandidates = true;
    answer.dtls = browser.dtls;
    answer.dtls.setup = sdp::DtlsSetup::Passive;
    for (auto& media : answer.medias)
    {
        media.ice = answer.ice;
        media.dtls = answer.dtls;
        media.direction = media.port && media.direction == sdp::MediaDirection::RecvOnly
            ? sdp::MediaDirection::SendOnly : sdp::MediaDirection::Inactive;
        media.ssrcs.clear();
        media.ssrcGroups.clear();
        if (media.direction == sdp::MediaDirection::SendOnly)
        {
            const auto source = std::find_if(browser.medias.begin(), browser.medias.end(),
                [&](const auto& item) { return item.mid == media.mid; });
            Require(source != browser.medias.end() && !source->ssrcs.empty(), "browser source not found");
            media.ssrcs = {source->ssrcs.front()};
            for (auto& extension : media.headerExtensions) extension.direction = sdp::MediaDirection::SendOnly;
        }
        else media.headerExtensions.clear();
    }
    return answer;
}

void RunRenegotiation()
{
    const auto baseline = utils::EndpointManager::Instance().Size();
    EventLoop loop(2);
    Require(loop.Start(), "renegotiation event loop did not start");
    config::AppConfig config;
    config.listen_ip = config.webrtc.public_ip = "127.0.0.1";
    config.udp_port = AvailablePort(SOCK_DGRAM);
    config.websocket_port = AvailablePort(SOCK_STREAM);
    config.webrtc.enabled = true;
    config.webrtc.token = "integration-secret";
    config.webrtc.max_sessions = 1;
    auto service = std::make_shared<server::WebRtcService>(&loop, config, nullptr);
    Require(service->Start(), "renegotiation service did not start");
    Client client(config.websocket_port);
    const Json initial = {{"type", "offer"}, {"token", config.webrtc.token}, {"sdp", Offer()}};
    const auto first = client.Request(initial);
    CheckAnswer(first, config.udp_port);
    const auto sessionId = first.at("session_id").get<uint64_t>();
    const auto endpoint = std::dynamic_pointer_cast<media::SfuEndpoint>(
        utils::EndpointManager::Instance().Find(sessionId));
    Require(endpoint != nullptr, "published endpoint not found");
    ::TrackInfo originalAudio, originalVideo;
    Require(endpoint->GetPublishedTrack("a", originalAudio) && endpoint->GetPublishedTrack("v", originalVideo),
        "initial audio/video tracks missing");
    const auto checkEndpoint = [&](size_t count, int payload)
    {
        Require(utils::EndpointManager::Instance().Find(sessionId).get() == endpoint.get(),
            "renegotiation replaced the published endpoint");
        Require(utils::EndpointManager::Instance().Size() == baseline + 1, "renegotiation changed session count");
        Require(endpoint->PublishedTrackCount() == count, "unexpected active track count");
        ::TrackInfo audio, video;
        if (count > 0)
            Require(endpoint->GetPublishedTrack("a", audio) && audio.track_index == originalAudio.track_index &&
                audio.payload_type == originalAudio.payload_type, "audio track identity changed");
        if (count > 1)
            Require(endpoint->GetPublishedTrack("v", video) && video.track_index == originalVideo.track_index &&
                video.payload_type == payload, "video track parameters or identity changed");
        else Require(!endpoint->GetPublishedTrack("v", video), "paused video track remained active");
    };
    auto local = ParseDescription(first, sdp::SdpType::Answer);
    const auto localOrigin = local.origin.sess_id;
    uint64_t localVersion = std::stoull(local.origin.sess_version);
    const auto checkDescription = [&](const Json& message, sdp::SdpType type)
    {
        Require(message.at("session_id").get<uint64_t>() == sessionId, "renegotiation changed session ID");
        auto description = ParseDescription(message, type);
        Require(description.origin.sess_id == localOrigin &&
            std::stoull(description.origin.sess_version) > localVersion, "server SDP origin did not advance");
        localVersion = std::stoull(description.origin.sess_version);
        Require(description.medias.size() == 2 && description.medias[0].mid == "a" &&
            description.medias[1].mid == "v", "renegotiation changed established m-line order");
        return description;
    };
    auto browser = ParseDescription(initial, sdp::SdpType::Offer);
    uint64_t browserVersion = 1;
    const auto sendOffer = [&](sdp::SdpSession offer)
    {
        offer.origin.sess_version = std::to_string(++browserVersion);
        return client.Request({{"type", "offer"}, {"token", config.webrtc.token},
            {"sdp", sdp::Sdp::Serialize(offer)}});
    };

    // The existing WS, endpoint and MID identities survive browser changes.
    auto codec = browser.medias[1].codecs.front();
    codec.payloadType = 105;
    codec.fmtp = "packetization-mode=1;profile-level-id=42e01e;level-asymmetry-allowed=0";
    sdp::Sdp::SetCodecs(browser.medias[1], {codec});
    auto reply = sendOffer(browser);
    Require(reply.value("type", "") == "answer", reply.dump().c_str());
    local = checkDescription(reply, sdp::SdpType::Answer);
    checkEndpoint(2, 105);
    ::TrackInfo updatedVideo;
    Require(endpoint->GetPublishedTrack("v", updatedVideo) && updatedVideo.fmtp != originalVideo.fmtp &&
        updatedVideo.fmtp.find("profile-level-id=42e01e") != std::string::npos,
        "reoffer did not update negotiated video format");
    browser.medias[1].direction = sdp::MediaDirection::Inactive;
    reply = sendOffer(browser);
    Require(reply.value("type", "") == "answer", reply.dump().c_str());
    local = checkDescription(reply, sdp::SdpType::Answer);
    checkEndpoint(1, 105);
    browser.medias[0].direction = sdp::MediaDirection::Inactive;
    reply = sendOffer(browser);
    Require(reply.value("type", "") == "answer", reply.dump().c_str());
    local = checkDescription(reply, sdp::SdpType::Answer);
    checkEndpoint(0, 105);
    for (auto& media : browser.medias) media.direction = sdp::MediaDirection::SendOnly;
    reply = sendOffer(browser);
    Require(reply.value("type", "") == "answer", reply.dump().c_str());
    local = checkDescription(reply, sdp::SdpType::Answer);
    checkEndpoint(2, 105);

    auto invalidOffer = browser;
    std::swap(invalidOffer.medias[0], invalidOffer.medias[1]);
    Require(sendOffer(invalidOffer).value("type", "") == "error", "reordered remote MIDs accepted");
    invalidOffer = browser;
    invalidOffer.ice.ufrag = "restart";
    for (auto& media : invalidOffer.medias) media.ice.ufrag = "restart";
    Require(sendOffer(invalidOffer).value("type", "") == "error", "remote ICE restart accepted");
    checkEndpoint(2, 105);

    // Business parameters stay pending until a matching, valid browser answer.
    auto desired = local.medias;
    codec = desired[1].codecs.front();
    codec.payloadType = 106;
    codec.fmtp = "packetization-mode=1;profile-level-id=42e016;level-asymmetry-allowed=0";
    sdp::Sdp::SetCodecs(desired[1], {codec});
    std::reverse(desired.begin(), desired.end());
    std::string error;
    const auto queueOffer = [&](const std::vector<sdp::SdpMedia>& medias)
    {
        const bool queued = service->Renegotiate(sessionId, medias, error);
        Require(queued, error.c_str());
        const auto message = client.Receive();
        Require(message.value("type", "") == "offer", message.dump().c_str());
        return message;
    };
    const auto pending = queueOffer(desired);
    auto offered = checkDescription(pending, sdp::SdpType::Offer);
    const auto negotiationId = pending.at("negotiation_id").get<std::string>();
    Require(negotiationId == offered.origin.sess_version, "offer correlation ID does not match origin version");
    checkEndpoint(2, 105);
    Require(!service->Renegotiate(sessionId, desired, error), "second pending business offer accepted");
    Require(sendOffer(browser).value("type", "") == "error", "remote glare offer accepted");
    auto answer = BrowserAnswer(offered, browser, ++browserVersion);
    Json answerRequest = {{"type", "answer"}, {"token", config.webrtc.token},
        {"negotiation_id", negotiationId}, {"sdp", sdp::Sdp::Serialize(answer)}};
    auto stale = answerRequest;
    stale["negotiation_id"] = "stale-" + negotiationId;
    Require(client.Request(stale).value("type", "") == "error", "stale answer correlation ID accepted");
    auto invalidAnswer = answer;
    codec = invalidAnswer.medias[1].codecs.front();
    codec.payloadType = 127;
    sdp::Sdp::SetCodecs(invalidAnswer.medias[1], {codec});
    auto invalidRequest = answerRequest;
    invalidRequest["sdp"] = sdp::Sdp::Serialize(invalidAnswer);
    Require(client.Request(invalidRequest).value("type", "") == "error", "answer introduced unoffered codec");
    invalidAnswer = answer;
    invalidAnswer.ice.ufrag = "restart";
    for (auto& media : invalidAnswer.medias) media.ice.ufrag = "restart";
    invalidRequest["sdp"] = sdp::Sdp::Serialize(invalidAnswer);
    Require(client.Request(invalidRequest).value("type", "") == "error", "answer changed existing ICE credentials");
    checkEndpoint(2, 105);
    Require(endpoint->GetPublishedTrack("v", updatedVideo) &&
        updatedVideo.fmtp.find("profile-level-id=42e01e") != std::string::npos,
        "rejected negotiation changed active video format");
    reply = client.Request(answerRequest);
    Require(reply.value("type", "") == "negotiated" && reply.at("session_id").get<uint64_t>() == sessionId &&
        reply.at("negotiation_id") == negotiationId, reply.dump().c_str());
    checkEndpoint(2, 106);
    Require(endpoint->GetPublishedTrack("v", updatedVideo) &&
        updatedVideo.fmtp.find("profile-level-id=42e016") != std::string::npos,
        "business answer did not commit video parameters");
    Require(client.Request(answerRequest).value("type", "") == "error", "completed answer replay accepted");
    checkEndpoint(2, 106);

    const auto rollbackOffer = queueOffer({offered.medias[0]});
    const auto rejected = checkDescription(rollbackOffer, sdp::SdpType::Offer);
    Require(rejected.medias[1].port == 0, "omitted MID was not preserved as a rejected m-line");
    checkEndpoint(2, 106);
    Json rollback = {{"type", "rollback"}, {"token", config.webrtc.token},
        {"negotiation_id", "stale"}};
    Require(client.Request(rollback).value("type", "") == "error", "stale rollback accepted");
    rollback["negotiation_id"] = rollbackOffer.at("negotiation_id");
    reply = client.Request(rollback);
    Require(reply.value("type", "") == "rolled_back" && reply.at("session_id").get<uint64_t>() == sessionId,
        "rollback response missing");
    checkEndpoint(2, 106);
    answerRequest["negotiation_id"] = rollbackOffer.at("negotiation_id");
    answerRequest["sdp"] = sdp::Sdp::Serialize(BrowserAnswer(rejected, browser, ++browserVersion));
    Require(client.Request(answerRequest).value("type", "") == "error", "answer committed after rollback");
    checkEndpoint(2, 106);

    const auto pauseOffer = queueOffer({});
    const auto paused = checkDescription(pauseOffer, sdp::SdpType::Offer);
    checkEndpoint(2, 106);
    answerRequest["negotiation_id"] = pauseOffer.at("negotiation_id");
    answerRequest["sdp"] = sdp::Sdp::Serialize(BrowserAnswer(paused, browser, ++browserVersion));
    Require(client.Request(answerRequest).value("type", "") == "negotiated", "business pause failed");
    checkEndpoint(0, 106);
    const auto resumeOffer = queueOffer(offered.medias);
    const auto resumed = checkDescription(resumeOffer, sdp::SdpType::Offer);
    checkEndpoint(0, 106);
    answerRequest["negotiation_id"] = resumeOffer.at("negotiation_id");
    answerRequest["sdp"] = sdp::Sdp::Serialize(BrowserAnswer(resumed, browser, ++browserVersion));
    Require(client.Request(answerRequest).value("type", "") == "negotiated", "business resume failed");
    checkEndpoint(2, 106);
    Require(client.Request({{"type", "close"}}).value("type", "") == "closed", "renegotiation session did not close");
    WaitForEndpoints(baseline);
    service->Stop();
    loop.Stop();
}

void Run()
{
    class ExistingEndpoint final : public utils::EndpointBase
    {
    public:
        ExistingEndpoint() : EndpointBase(NextEndpointId(), "existing-protocol") {}
        bool Start() override { return true; }
        void Stop() override {}
    };
    // RTSP uses this allocator and registry before WebRTC may receive an offer.
    const auto existing = std::make_shared<ExistingEndpoint>();
    Require(utils::EndpointManager::Instance().Add(existing), "existing endpoint registration failed");
    EventLoop loop(2);
    Require(loop.Start(), "event loop did not start");
    config::AppConfig config;
    config.listen_ip = config.webrtc.public_ip = "127.0.0.1";
    config.udp_port = AvailablePort(SOCK_DGRAM);
    config.websocket_port = AvailablePort(SOCK_STREAM);
    config.webrtc.enabled = true;
    config.webrtc.token = "integration-secret";
    config.webrtc.max_sessions = 1;
    const auto baseline = utils::EndpointManager::Instance().Size();

    auto invalid = config;
    invalid.webrtc.public_ip = "0.0.0.0";
    Require(!std::make_shared<server::WebRtcService>(&loop, invalid, nullptr)->Start(), "wildcard candidate accepted");
    invalid.webrtc.public_ip = "::1";
    Require(!std::make_shared<server::WebRtcService>(&loop, invalid, nullptr)->Start(), "address family mismatch accepted");
    invalid = config;
    invalid.webrtc.token.clear();
    Require(!std::make_shared<server::WebRtcService>(&loop, invalid, nullptr)->Start(), "empty token accepted");

    // A failed WebSocket bind must release the UDP listener opened before it.
    {
        Socket busy(SOCK_STREAM);
        Require(busy.Bind(config.websocket_port) && ::listen(busy.Get(), 1) == 0, "could not occupy WS port");
        auto service = std::make_shared<server::WebRtcService>(&loop, config, nullptr);
        Require(!service->Start(), "occupied WS port accepted");
        Socket released(SOCK_DGRAM);
        Require(released.Bind(config.udp_port), "partial startup leaked UDP listener");
    }

    auto service = std::make_shared<server::WebRtcService>(&loop, config, nullptr);
    Require(service->Start() && service->Start(), "service start failed");
    Client first(config.websocket_port), second(config.websocket_port);
    const Json offer = {{"type", "offer"}, {"token", config.webrtc.token}, {"sdp", Offer()}};
    auto bad = offer;
    bad["token"] = "wrong-secret";
    Require(first.Request(bad).value("type", "") == "error", "wrong token accepted");
    Require(first.RequestText("{").value("type", "") == "error", "invalid JSON accepted");
    bad = offer;
    bad["sdp"] = "not SDP";
    Require(first.Request(bad).value("type", "") == "error", "invalid SDP accepted");
    bad["sdp"] = std::string(65537, 'x');
    Require(first.Request(bad).value("type", "") == "error", "oversized SDP accepted");
    Require(utils::EndpointManager::Instance().Size() == baseline, "rejected request allocated endpoint");
    CheckAnswer(first.Request(offer), config.udp_port);
    Require(utils::EndpointManager::Instance().Size() == baseline + 1, "accepted offer did not register endpoint");
    Require(first.Request(offer).value("type", "") == "error", "duplicate offer accepted");
    Require(second.Request(offer).value("type", "") == "error", "session limit exceeded");
    Require(first.RequestText("{").value("type", "") == "error", "invalid JSON accepted on active session");
    Require(utils::EndpointManager::Instance().Size() == baseline + 1, "invalid message removed active session");
    Require(first.Request({{"type", "close"}}).value("type", "") == "closed", "close response missing");
    WaitForEndpoints(baseline);
    CheckAnswer(second.Request(offer), config.udp_port);
    second.Close();
    WaitForEndpoints(baseline);
    service->Stop();
    service->Stop();
    loop.Stop();

    Require(loop.Start() && service->Start(), "service did not restart with new scheduler");
    Client restarted(config.websocket_port);
    CheckAnswer(restarted.Request(offer), config.udp_port);
    service->Stop();
    WaitForEndpoints(baseline);
    loop.Stop();
    Socket released(SOCK_DGRAM);
    Require(released.Bind(config.udp_port), "service stop leaked UDP listener");
    Require(utils::EndpointManager::Instance().Find(existing->Id()) == existing,
        "WebRTC replaced or removed another protocol's endpoint");
    utils::EndpointManager::Instance().Remove(existing->Id());
}
}

int main()
{
    std::signal(SIGPIPE, SIG_IGN);
    try
    {
        Run();
        RunRenegotiation();
        std::cout << "PASS WebRTC service signaling, renegotiation, admission, cleanup and restart\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL WebRTC service: " << error.what() << '\n';
        return 1;
    }
}
