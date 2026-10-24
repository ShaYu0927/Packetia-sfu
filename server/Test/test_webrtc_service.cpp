#include "WebRtcService.h"
#include "EventLoop.h"
#include "EndpointBase.h"
#include "Sdp.h"
#include "SfuEndpoint.h"
#include "third/nlohmann/json.hpp"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

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

constexpr const char* kApiKey = "integration-key";
constexpr const char* kOtherApiKey = "other-integration-key";
constexpr const char* kApiSecret = "test-secret-with-at-least-32-bytes";

int64_t UnixNow()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string Base64Url(const unsigned char* bytes, size_t size)
{
    std::string result(4 * ((size + 2) / 3), '\0');
    const int written = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(result.data()), bytes,
        static_cast<int>(size));
    Require(written >= 0, "base64 encoding failed");
    result.resize(static_cast<size_t>(written));
    while (!result.empty() && result.back() == '=') result.pop_back();
    std::replace(result.begin(), result.end(), '+', '-');
    std::replace(result.begin(), result.end(), '/', '_');
    return result;
}

std::string RoomToken(const std::string& identity, const std::string& room,
    bool publish = true, bool subscribe = true, int64_t expires = 0,
    const std::string& issuer = kApiKey, const std::string& secret = kApiSecret)
{
    const auto now = UnixNow();
    const auto expiration = expires ? expires : now + 3600;
    const auto issued = std::min(now, expiration - 60);
    const auto header = Json{{"alg", "HS256"}, {"typ", "JWT"}}.dump();
    const auto claims = Json{{"iss", issuer}, {"sub", identity}, {"aud", "packetia-webrtc"},
        {"iat", issued}, {"nbf", issued - 1}, {"exp", expiration},
        {"name", "Display " + identity}, {"video", {{"room", room}, {"roomJoin", true},
            {"canPublish", publish}, {"canSubscribe", subscribe}}}}.dump();
    const auto encode = [](const std::string& value) {
        return Base64Url(reinterpret_cast<const unsigned char*>(value.data()), value.size());
    };
    const auto signingInput = encode(header) + "." + encode(claims);
    unsigned char signature[EVP_MAX_MD_SIZE];
    unsigned int signatureSize = 0;
    Require(HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
        reinterpret_cast<const unsigned char*>(signingInput.data()), signingInput.size(),
        signature, &signatureSize) != nullptr, "test token signing failed");
    return signingInput + "." + Base64Url(signature, signatureSize);
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

Json Join(Client& client, const std::string& identity, const std::string& room,
    bool publish = true, bool subscribe = true, int64_t expires = 0)
{
    const auto reply = client.Request({{"type", "join"}, {"room_id", room},
        {"token", RoomToken(identity, room, publish, subscribe, expires)}});
    Require(reply.value("type", "") == "joined", reply.dump().c_str());
    Require(reply.value("subject_id", "") == identity, "join did not bind verified identity");
    return reply;
}

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

void RunNegotiationGuards()
{
    const auto baseline = utils::EndpointManager::Instance().Size();
    EventLoop loop(2);
    Require(loop.Start(), "negotiation guard event loop did not start");
    config::AppConfig config;
    config.listen_ip = config.webrtc.public_ip = "127.0.0.1";
    config.udp_port = AvailablePort(SOCK_DGRAM);
    config.websocket_port = AvailablePort(SOCK_STREAM);
    config.webrtc.enabled = true;
    config.webrtc.api_keys = {{kApiKey, kApiSecret}};
    auto service = std::make_shared<server::WebRtcService>(&loop, config, nullptr);
    Require(service->Start(), "negotiation guard service did not start");
    Client client(config.websocket_port);
    const Json reoffer = {{"type", "offer"}, {"sdp", Offer()}};
    Require(client.Request(reoffer).value("type", "") == "error", "bare offer created session");
    Join(client, "guard-publisher", "negotiation");
    Require(client.Request(reoffer).value("type", "") == "error", "offer bypassed explicit publish admission");
    const auto initial = client.Request({{"type", "publish"}, {"sdp", Offer()}});
    CheckAnswer(initial, config.udp_port);
    const auto sessionId = initial.at("session_id").get<uint64_t>();
    const auto endpoint = std::dynamic_pointer_cast<media::SfuEndpoint>(
        utils::EndpointManager::Instance().Find(sessionId));
    Require(endpoint && endpoint->PublishedTrackCount() == 2, "initial room publication missing");
    Require(client.Request(reoffer).value("type", "") == "error", "room reoffer changed established routes");
    std::string error;
    const auto description = ParseDescription(initial, sdp::SdpType::Answer);
    Require(!service->Renegotiate(sessionId, description.medias, error), "unsupported room renegotiation queued");
    Require(client.Request({{"type", "answer"}, {"negotiation_id", "unsolicited"}, {"sdp", Offer()}})
        .value("type", "") == "error", "unsolicited answer accepted");
    Require(client.Request({{"type", "rollback"}, {"negotiation_id", "unsolicited"}})
        .value("type", "") == "error", "unsolicited rollback accepted");
    Require(utils::EndpointManager::Instance().Find(sessionId).get() == endpoint.get() &&
        utils::EndpointManager::Instance().Size() == baseline + 1 && endpoint->PublishedTrackCount() == 2,
        "rejected room negotiation mutated endpoint");
    Require(client.Request({{"type", "tracks"}}).at("tracks").size() == 2,
        "rejected room negotiation mutated advertised tracks");
    Require(client.Request({{"type", "close"}}).value("type", "") == "closed", "room close failed");
    WaitForEndpoints(baseline);
    service->Stop();
    loop.Stop();
}

void RunRoom()
{
    const auto baseline = utils::EndpointManager::Instance().Size();
    EventLoop loop(2);
    Require(loop.Start(), "room event loop did not start");
    config::AppConfig config;
    config.listen_ip = config.webrtc.public_ip = "127.0.0.1";
    config.udp_port = AvailablePort(SOCK_DGRAM);
    config.websocket_port = AvailablePort(SOCK_STREAM);
    config.webrtc.enabled = true;
    config.webrtc.api_keys = {{kApiKey, kApiSecret}, {kOtherApiKey, kApiSecret}};
    config.webrtc.max_sessions = 6;
    config.webrtc.reconnect_timeout_ms = 1200;
    auto service = std::make_shared<server::WebRtcService>(&loop, config, nullptr);
    Require(service->Start(), "room service did not start");
    Client a(config.websocket_port), b(config.websocket_port), outsider(config.websocket_port), secondPublisher(config.websocket_port);
    Require(a.Request({{"type", "join"}, {"room_id", "demo"}, {"token", "wrong"}}).value("type", "") == "error",
        "unauthorized room join accepted");
    const auto publisherJoin = Join(a, "publisher", "demo");
    const auto publisherId = publisherJoin.at("participant_id").get<std::string>();
    const auto viewerJoin = Join(b, "viewer", "demo", false, true);
    const auto outsiderJoin = Join(outsider, "outsider", "other");
    Join(secondPublisher, "second-publisher", "demo");
    const auto publication = a.Request({{"type", "publish"}, {"sdp", Offer()}});
    CheckAnswer(publication, config.udp_port);
    const auto source = std::dynamic_pointer_cast<media::SfuEndpoint>(utils::EndpointManager::Instance().Find(
        publication.at("session_id").get<uint64_t>()));
    Require(source && source->PublishedTrackCount() == 2, "room source endpoint missing");
    const auto list = [&](Client& client) {
        return client.Request({{"type", "tracks"}}).at("tracks");
    };
    const auto tracks = list(b);
    Require(tracks.size() == 2 && list(outsider).empty(), "room track visibility incorrect");
    auto browser = ParseDescription({{"sdp", Offer()}}, sdp::SdpType::Offer);
    browser.bundle.mids = {"down-a", "down-v"};
    Json bindings = Json::array();
    for (auto& media : browser.medias)
    {
        const auto sourceMid = media.mid;
        media.mid = "down-" + sourceMid;
        media.direction = sdp::MediaDirection::RecvOnly;
        media.ssrcs.clear(); media.ssrcGroups.clear();
        auto codec = media.codecs.front();
        codec.payloadType = media.media == "video" ? 120 : 112;
        sdp::Sdp::SetCodecs(media, {codec});
        bindings.push_back({{"track_id", publisherId + ":" + sourceMid}, {"mid", media.mid}});
    }
    Json subscription = {{"type", "subscribe"},
        {"sdp", sdp::Sdp::Serialize(browser)}, {"tracks", bindings}};
    Require(outsider.Request(subscription).value("type", "") == "error", "cross-room subscription accepted");
    auto duplicate = subscription;
    duplicate["tracks"].push_back(bindings.front());
    Require(b.Request(duplicate).value("type", "") == "error", "duplicate binding accepted");
    const auto reply = b.Request(subscription);
    Require(reply.value("type", "") == "answer", reply.dump().c_str());
    const auto answer = ParseDescription(reply, sdp::SdpType::Answer);
    Require(answer.medias.size() == 2, "downstream media missing");
    for (const auto& media : answer.medias)
        Require(media.direction == sdp::MediaDirection::SendOnly && media.codecs.size() == 1 && media.ssrcs.size() == 1 &&
            media.codecs.front().payloadType == (media.media == "video" ? 120 : 112), "downstream negotiation reused upstream identifiers");
    const auto destination = std::dynamic_pointer_cast<media::SfuEndpoint>(utils::EndpointManager::Instance().Find(
        reply.at("session_id").get<uint64_t>()));
    Require(destination && destination->SubscriptionCount() == 2, "room did not create real forwarding routes");
    const auto second = secondPublisher.Request({{"type", "publish"}, {"sdp", Offer()}});
    CheckAnswer(second, config.udp_port);
    Require(list(b).size() == 4, "same MIDs from another publisher collided in room");
    Require(b.Request(subscription).value("type", "") == "error", "duplicate room media session accepted");
    Require(a.Request({{"type", "offer"}, {"sdp", Offer()}}).value("type", "") == "error",
        "legacy reoffer changed room routes");
    Client resumedPublisher(config.websocket_port);
    Json resume = {{"type", "resume"}, {"token", RoomToken("publisher", "demo")}, {"room_id", "demo"},
        {"participant_id", publisherId}, {"resume_token", publisherJoin.at("resume_token")}};
    Require(resumedPublisher.Request(resume).value("type", "") == "error", "active participant takeover accepted");
    a.Close();
    std::this_thread::sleep_for(50ms);
    auto invalidRecovery = resume;
    invalidRecovery["resume_token"] = std::string(64, '0');
    Require(resumedPublisher.Request(invalidRecovery).value("type", "") == "error", "wrong recovery token accepted");
    invalidRecovery = resume; invalidRecovery["room_id"] = "other";
    Require(resumedPublisher.Request(invalidRecovery).value("type", "") == "error", "cross-room recovery accepted");
    const auto recovered = resumedPublisher.Request(resume);
    Require(recovered.value("type", "") == "resumed" && recovered.at("participant_id") == publisherId &&
        recovered.at("signaling_generation") == 2 && recovered.at("ice_restart") == true, "publisher recovery failed");
    Require(list(b).size() == 4 && destination->SubscriptionCount() == 2, "signaling disconnect lost room routes");
    auto restartOffer = ParseDescription({{"sdp", Offer()}}, sdp::SdpType::Offer);
    restartOffer.origin.sess_version = "2";
    restartOffer.ice.ufrag = "newpeer";
    restartOffer.ice.pwd = std::string(24, 'n');
    for (auto& media : restartOffer.medias) media.ice = restartOffer.ice;
    const auto restarted = resumedPublisher.Request({{"type", "restart_ice"},
        {"sdp", sdp::Sdp::Serialize(restartOffer)}});
    CheckAnswer(restarted, config.udp_port);
    Require(restarted.at("session_id") == publication.at("session_id"), "ICE restart replaced media endpoint");
    const auto previousIce = ParseDescription(publication, sdp::SdpType::Answer).medias.front().ice;
    const auto nextIce = ParseDescription(restarted, sdp::SdpType::Answer).medias.front().ice;
    Require(previousIce.ufrag != nextIce.ufrag && previousIce.pwd != nextIce.pwd, "server ICE credentials did not rotate");
    Require(destination->SubscriptionCount() == 2 && list(b).size() == 4, "ICE restart removed forwarding routes");
    Require(resumedPublisher.Request({{"type", "restart_ice"},
        {"sdp", sdp::Sdp::Serialize(restartOffer)}}).value("type", "") == "error", "unchanged ICE credentials accepted");

    b.Close();
    std::this_thread::sleep_for(50ms);
    Client resumedViewer(config.websocket_port);
    const auto viewerRecovery = resumedViewer.Request({{"type", "resume"},
        {"token", RoomToken("viewer", "demo", false, true)},
        {"room_id", "demo"}, {"participant_id", viewerJoin.at("participant_id")}, {"resume_token", viewerJoin.at("resume_token")}});
    Require(viewerRecovery.value("type", "") == "resumed", "viewer recovery failed");
    browser.origin.sess_version = "2";
    browser.ice.ufrag = "newviewer"; browser.ice.pwd = std::string(24, 'v');
    for (auto& media : browser.medias) media.ice = browser.ice;
    const auto viewerRestart = resumedViewer.Request({{"type", "restart_ice"},
        {"sdp", sdp::Sdp::Serialize(browser)}});
    Require(viewerRestart.value("type", "") == "answer" && viewerRestart.at("session_id") == reply.at("session_id"),
        "viewer ICE restart failed");
    const auto viewerAnswer = ParseDescription(viewerRestart, sdp::SdpType::Answer);
    for (size_t i = 0; i < answer.medias.size(); ++i)
        Require(viewerAnswer.medias[i].ssrcs.front().ssrc == answer.medias[i].ssrcs.front().ssrc, "restart changed downstream SSRC");
    outsider.Close();
    std::this_thread::sleep_for(50ms);
    Client resumedOutsider(config.websocket_port);
    const auto idleRecovery = resumedOutsider.Request({{"type", "resume"},
        {"token", RoomToken("outsider", "other")},
        {"room_id", "other"}, {"participant_id", outsiderJoin.at("participant_id")},
        {"resume_token", outsiderJoin.at("resume_token")}});
    Require(idleRecovery.value("type", "") == "resumed" && idleRecovery.at("ice_restart") == false &&
        list(resumedOutsider).empty(), "participant without media could not recover its room");
    resumedPublisher.Close();
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (list(resumedViewer).size() != 2)
    {
        Require(std::chrono::steady_clock::now() < deadline, "publisher disconnect left stale room tracks");
        std::this_thread::sleep_for(5ms);
    }
    Require(destination->SubscriptionCount() == 0, "publisher disconnect retained downstream routes");
    Client expiredPublisher(config.websocket_port);
    Require(expiredPublisher.Request(resume).value("type", "") == "error", "expired participant recovered");
    Require(resumedViewer.Request({{"type", "leave"}}).value("type", "") == "closed", "explicit leave failed");
    Client departedViewer(config.websocket_port);
    Require(departedViewer.Request({{"type", "resume"}, {"token", RoomToken("viewer", "demo", false, true)},
        {"room_id", "demo"}, {"participant_id", viewerJoin.at("participant_id")},
        {"resume_token", viewerJoin.at("resume_token")}}).value("type", "") == "error", "explicit leave allowed resume");
    secondPublisher.Close(); resumedViewer.Close(); resumedOutsider.Close();
    WaitForEndpoints(baseline);
    service->Stop(); loop.Stop();
}

void RunAuthorization()
{
    const auto baseline = utils::EndpointManager::Instance().Size();
    EventLoop loop(2);
    Require(loop.Start(), "authorization event loop did not start");
    config::AppConfig config;
    config.listen_ip = config.webrtc.public_ip = "127.0.0.1";
    config.udp_port = AvailablePort(SOCK_DGRAM);
    config.websocket_port = AvailablePort(SOCK_STREAM);
    config.webrtc.enabled = true;
    config.webrtc.api_keys = {{kApiKey, kApiSecret}, {kOtherApiKey, kApiSecret}};
    config.webrtc.max_sessions = 8;
    config.webrtc.reconnect_timeout_ms = 5000;
    auto service = std::make_shared<server::WebRtcService>(&loop, config, nullptr);
    Require(service->Start(), "authorization service did not start");
    Client observer(config.websocket_port), stranger(config.websocket_port);
    Json admission = {{"type", "join"}, {"room_id", "secure"}};
    Require(observer.Request(admission).value("type", "") == "error", "missing JWT accepted");
    admission["token"] = "not.a.jwt";
    Require(observer.Request(admission).value("type", "") == "error", "malformed JWT accepted");
    admission["token"] = RoomToken("observer", "secure", false, false, 0, kApiKey, "incorrect signing key");
    Require(observer.Request(admission).value("type", "") == "error", "forged signature accepted");
    admission["token"] = RoomToken("observer", "secure", false, false, UnixNow() - 120);
    Require(observer.Request(admission).value("code", "") == "token_expired", "expired admission token accepted");
    admission["token"] = RoomToken("observer", "different-room", false, false);
    Require(observer.Request(admission).value("type", "") == "error", "JWT for another room accepted");
    Require(stranger.Request({{"type", "offer"}, {"sdp", Offer()},
        {"token", RoomToken("stranger", "secure")}}).value("type", "") == "error",
        "valid JWT in bare offer bypassed room admission");
    Require(stranger.Request({{"type", "tracks"}}).value("type", "") == "error",
        "unbound connection accessed room tracks");
    Require(utils::EndpointManager::Instance().Size() == baseline, "denied admissions allocated endpoints");

    admission["token"] = RoomToken("observer", "secure", false, false);
    admission["subject_id"] = "spoofed-subject";
    const auto joined = observer.Request(admission);
    Require(joined.value("type", "") == "joined" && joined.value("subject_id", "") == "observer",
        "client-supplied identity replaced signed identity");
    Require(observer.Request({{"type", "tracks"}}).value("type", "") == "tracks",
        "bound participant cannot list tracks without resending JWT");
    const Json publication = {{"type", "publish"}, {"sdp", Offer()}};
    Require(observer.Request(publication).value("code", "") == "forbidden", "read-only participant published");
    Require(observer.Request({{"type", "subscribe"}, {"sdp", Offer()}, {"tracks", Json::array()}})
        .value("code", "") == "forbidden", "subscribe permission was not checked before processing the request");
    Require(observer.Request({{"type", "offer"}, {"sdp", Offer()}}).value("type", "") == "error",
        "read-only participant bypassed publish permission through offer");
    auto injected = publication;
    injected["token"] = RoomToken("privileged-user", "secure");
    Require(observer.Request(injected).value("code", "") == "forbidden", "per-message token replaced bound permissions");
    Require(utils::EndpointManager::Instance().Size() == baseline, "denied media action allocated endpoint");
    Require(stranger.Request({{"type", "join"}, {"room_id", "secure"},
        {"token", RoomToken("observer", "secure", false, false)}}).value("type", "") == "error",
        "duplicate room identity admitted");
    Client otherRoom(config.websocket_port);
    Join(otherRoom, "observer", "other-secure", false, false);

    observer.Close();
    std::this_thread::sleep_for(50ms);
    Require(stranger.Request({{"type", "join"}, {"room_id", "secure"},
        {"token", RoomToken("observer", "secure", false, false)}}).value("type", "") == "error",
        "duplicate identity admitted during recovery grace");
    Json recovery = {{"type", "resume"}, {"room_id", "secure"},
        {"participant_id", joined.at("participant_id")}, {"resume_token", joined.at("resume_token")}};
    Require(stranger.Request(recovery).value("type", "") == "error", "resume without JWT accepted");
    recovery["token"] = RoomToken("other-user", "secure", false, false);
    Require(stranger.Request(recovery).value("type", "") == "error", "different subject resumed participant");
    recovery["token"] = RoomToken("observer", "secure", false, false, 0, kOtherApiKey);
    Require(stranger.Request(recovery).value("type", "") == "error", "different issuer resumed participant");
    recovery["token"] = RoomToken("observer", "secure", true, false);
    Require(stranger.Request(recovery).value("type", "") == "error", "resume changed original publish grant");
    recovery["token"] = RoomToken("observer", "secure", false, true);
    Require(stranger.Request(recovery).value("type", "") == "error", "resume changed original subscribe grant");
    recovery["token"] = RoomToken("observer", "secure", false, false, UnixNow() - 120);
    Require(stranger.Request(recovery).value("code", "") == "token_expired", "expired JWT resumed participant");
    recovery["token"] = RoomToken("observer", "secure", false, false);
    const auto resumed = stranger.Request(recovery);
    Require(resumed.value("type", "") == "resumed" && resumed.value("subject_id", "") == "observer" &&
        resumed.at("participant_id") == joined.at("participant_id"), "same identity and grants could not resume");
    Require(stranger.Request(publication).value("code", "") == "forbidden", "resume lost publish restriction");

    // Admission-token expiry does not silently revoke an established participant.
    Client shortLived(config.websocket_port);
    const auto expiry = UnixNow() + 2;
    Join(shortLived, "short-lived", "short-token", true, true, expiry);
    while (UnixNow() <= expiry) std::this_thread::sleep_for(25ms);
    Require(shortLived.Request({{"type", "tracks"}}).value("type", "") == "tracks",
        "admission expiry invalidated a connected participant");
    CheckAnswer(shortLived.Request(publication), config.udp_port);
    Require(shortLived.Request({{"type", "leave"}}).value("type", "") == "closed",
        "expired admission token prevented participant from leaving");
    Require(stranger.Request({{"type", "leave"}}).value("type", "") == "closed", "observer leave failed");
    Require(otherRoom.Request({{"type", "leave"}}).value("type", "") == "closed", "other-room leave failed");
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
    config.webrtc.api_keys = {{kApiKey, kApiSecret}, {kOtherApiKey, kApiSecret}};
    config.webrtc.max_sessions = 1;
    const auto baseline = utils::EndpointManager::Instance().Size();
    config.webrtc.reconnect_timeout_ms = 200;

    auto invalid = config;
    invalid.webrtc.public_ip = "0.0.0.0";
    Require(!std::make_shared<server::WebRtcService>(&loop, invalid, nullptr)->Start(), "wildcard candidate accepted");
    invalid.webrtc.public_ip = "::1";
    Require(!std::make_shared<server::WebRtcService>(&loop, invalid, nullptr)->Start(), "address family mismatch accepted");
    invalid = config;
    invalid.webrtc.api_keys.clear();
    Require(!std::make_shared<server::WebRtcService>(&loop, invalid, nullptr)->Start(), "missing API keys accepted");

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
    const Json offer = {{"type", "offer"}, {"sdp", Offer()}};
    Json bad = {{"type", "join"}, {"room_id", "admission"}, {"token", "wrong-secret"}};
    Require(first.Request(bad).value("type", "") == "error", "wrong token accepted");
    Require(first.Request(offer).value("type", "") == "error", "bare offer bypassed room admission");
    Require(first.RequestText("{").value("type", "") == "error", "invalid JSON accepted");
    Join(first, "first", "admission");
    const Json publication = {{"type", "publish"}, {"sdp", Offer()}};
    bad = publication;
    bad["sdp"] = "not SDP";
    Require(first.Request(bad).value("type", "") == "error", "invalid SDP accepted");
    bad["sdp"] = std::string(65537, 'x');
    Require(first.Request(bad).value("type", "") == "error", "oversized SDP accepted");
    Require(utils::EndpointManager::Instance().Size() == baseline, "rejected request allocated endpoint");
    CheckAnswer(first.Request(publication), config.udp_port);
    Require(utils::EndpointManager::Instance().Size() == baseline + 1, "accepted publication did not register endpoint");
    Require(first.Request(offer).value("type", "") == "error", "room reoffer bypassed negotiation guard");
    Require(second.Request({{"type", "join"}, {"room_id", "admission"},
        {"token", RoomToken("second", "admission")}}).value("type", "") == "error", "session limit exceeded");
    Require(first.RequestText("{").value("type", "") == "error", "invalid JSON accepted on active session");
    Require(utils::EndpointManager::Instance().Size() == baseline + 1, "invalid message removed active session");
    Require(first.Request({{"type", "close"}}).value("type", "") == "closed", "close response missing");
    WaitForEndpoints(baseline);
    Join(second, "second", "admission");
    auto shared = ParseDescription(offer, sdp::SdpType::Offer);
    shared.origin.sess_version = "2";
    auto screen = shared.medias[1];
    screen.mid = "screen";
    screen.ssrcs = {{3001, {{"cname", "screen"}}}};
    screen.ssrcGroups.clear();
    shared.medias.push_back(screen);
    shared.bundle.mids.push_back(screen.mid);
    const auto sharedReply = second.Request({{"type", "publish"},
        {"sdp", sdp::Sdp::Serialize(shared)}});
    Require(sharedReply.value("type", "") == "answer", sharedReply.dump().c_str());
    const auto sharedAnswer = ParseDescription(sharedReply, sdp::SdpType::Answer);
    Require(sharedAnswer.medias.size() == 3 && sharedAnswer.medias[1].port != 0 && sharedAnswer.medias[2].port != 0 &&
        sharedAnswer.medias[1].codecs.front().payloadType == sharedAnswer.medias[2].codecs.front().payloadType,
        "camera and screen must negotiate the same primary PT on separate MIDs");
    const auto endpoint = std::dynamic_pointer_cast<media::SfuEndpoint>(
        utils::EndpointManager::Instance().Find(sharedReply.at("session_id").get<uint64_t>()));
    Require(endpoint && endpoint->PublishedTrackCount() == 3, "shared payload publication lost a track");
    second.Close();
    WaitForEndpoints(baseline);
    service->Stop();
    service->Stop();
    loop.Stop();

    Require(loop.Start() && service->Start(), "service did not restart with new scheduler");
    Client restarted(config.websocket_port);
    Join(restarted, "restarted", "admission");
    CheckAnswer(restarted.Request(publication), config.udp_port);
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
        RunNegotiationGuards();
        RunRoom();
        RunAuthorization();
        std::cout << "PASS WebRTC service JWT authorization, rooms, subscriptions, renegotiation, admission, cleanup and restart\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL WebRTC service: " << error.what() << '\n';
        return 1;
    }
}
