#include "WebRtcService.h"
#include "signaling/SignalSessionCoordinator.h"
#include "JwtAuthenticator.h"
#include "KeyProvider.h"
#include "RoomAccessPolicy.h"
#include "Room.h"
#include "Participant.h"
#include <deque>
#include "EventLoop.h"
#include "EndpointBase.h"
#include "Sdp.h"
#include "SfuEndpoint.h"
#include "MediaStreamRegistry.h"
#include "Stun.h"
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
#include <future>
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
    const std::string& issuer = kApiKey, const std::string& secret = kApiSecret,
    bool admin = false, bool updateOwn = false, const std::string& assignment = "")
{
    const auto now = UnixNow();
    const auto expiration = expires ? expires : now + 3600;
    const auto issued = std::min(now, expiration - 60);
    const auto header = Json{{"alg", "HS256"}, {"typ", "JWT"}}.dump();
    auto claims = Json{{"iss", issuer}, {"sub", identity}, {"aud", "packetia-webrtc"},
        {"iat", issued}, {"nbf", issued - 1}, {"exp", expiration},
        {"name", "Display " + identity}, {"video", {{"room", room}, {"roomJoin", true},
            {"canPublish", publish}, {"canSubscribe", subscribe},
            {"roomAdmin", admin}, {"canUpdateOwnMetadata", updateOwn}}}};
    if (!assignment.empty()) claims["assignment_id"] = assignment;
    const auto encode = [](const std::string& value) {
        return Base64Url(reinterpret_cast<const unsigned char*>(value.data()), value.size());
    };
    const auto signingInput = encode(header) + "." + encode(claims.dump());
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
    bool HasReadable()
    {
        pollfd descriptor{socket_.Get(), POLLIN, 0};
        return ::poll(&descriptor, 1, 0) > 0;
    }

    void ExpectClosed()
    {
        const auto header = Read(2);
        Require((static_cast<uint8_t>(header[0]) & 15) == 8 &&
            static_cast<uint8_t>(header[1]) == 2, "expected normal WebSocket close after final response");
        const auto status = Read(2);
        Require(static_cast<uint8_t>(status[0]) == 3 && static_cast<uint8_t>(status[1]) == 232,
            "expected normal close status");
        Close();
    }

    Json Request(const Json& request) { return RequestText(request.dump()); }

    Json RequestText(const std::string& payload)
    {
        Send(1, payload);
        return Receive();
    }

    Json Receive()
    {
        for (;;)
        {
            auto message = ReceiveFrame();
            if (!StoreNotification(message)) return message;
        }
    }

    bool StoreNotification(const Json& message)
    {
        const auto type = message.value("type", "");
        if (type == "tracks_changed") notifications_.push_back(message);
        else if (type == "participants_changed") member_notifications_.push_back(message);
        else if (type == "permissions_changed") permission_notifications_.push_back(message);
        else return false;
        return true;
    }

    Json ReceiveNotification(const std::string& type, std::deque<Json>& queue)
    {
        while (queue.empty())
        {
            auto message = ReceiveFrame();
            Require(StoreNotification(message), ("expected unsolicited " + type + " notification").c_str());
        }
        auto message = std::move(queue.front());
        queue.pop_front();
        return message;
    }
    Json ReceiveTrackChange() { return ReceiveNotification("tracks_changed", notifications_); }
    Json ReceiveMemberChange() { return ReceiveNotification("participants_changed", member_notifications_); }
    Json ReceivePermissionsChange() { return ReceiveNotification("permissions_changed", permission_notifications_); }
    std::size_t PendingTrackChanges() const { return notifications_.size(); }

    Json ReceiveFrame()
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

public:
    // 允许测试流水线请求，不等待上一条响应。
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

private:
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
    std::deque<Json> notifications_, member_notifications_, permission_notifications_;
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

void RunSignalingLifecycle()
{
    EventLoop loop(1);
    Require(loop.Start(), "signaling lifecycle loop did not start");
    config::AppConfig config;
    config.listen_ip = config.webrtc.public_ip = "127.0.0.1";
    config.udp_port = AvailablePort(SOCK_DGRAM);
    config.websocket_port = AvailablePort(SOCK_STREAM);
    config.webrtc.enabled = true;
    config.webrtc.api_keys = {{kApiKey, kApiSecret}};
    config.webrtc.admission_timeout_ms = 500;
    config.webrtc.reconnect_timeout_ms = 250;
    auto service = std::make_shared<server::WebRtcService>(&loop, config, nullptr);
    Require(service->Start(), "signaling lifecycle service did not start");
    const auto wait = [&](auto predicate)
    {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!predicate(service->GetStats()))
        {
            Require(std::chrono::steady_clock::now() < deadline, "signaling lifecycle stats timed out");
            std::this_thread::sleep_for(2ms);
        }
    };

    Client idle(config.websocket_port);
    Require(idle.Request({{"type", "join"}, {"token", "bad"}, {"room_id", "lifecycle"}})
        .value("type", "") == "error", "invalid admission unexpectedly succeeded");
    Require(idle.Receive().value("error", "") == "Room admission timed out", "admission timeout response lost");
    idle.ExpectClosed();
    wait([](const auto& stats) { return stats.signaling_closed == 1; });
    auto stats = service->GetStats();
    Require(stats.admission_timeouts == 1 && stats.join_attempts == 1 && stats.join_failed == 1 &&
        stats.active_signaling == 0 && stats.pending_admissions == 0, "admission timeout counted incorrectly");

    Client original(config.websocket_port);
    const auto joined = Join(original, "lifecycle", "lifecycle");
    original.Close();
    wait([](const auto& state) { return state.reconnecting_participants == 1; });
    Client recovered(config.websocket_port);
    Json resume = {{"type", "resume"}, {"room_id", "lifecycle"}, {"token", RoomToken("lifecycle", "lifecycle")},
        {"participant_id", joined.at("participant_id")}, {"resume_token", "wrong"}};
    Require(recovered.Request(resume).value("type", "") == "error", "invalid resume unexpectedly succeeded");
    resume["resume_token"] = joined.at("resume_token");
    Require(recovered.Request(resume).value("type", "") == "resumed", "valid resume failed");
    // Admission timers must be cancelled for joined and resumed connections.
    std::this_thread::sleep_for(650ms);
    stats = service->GetStats();
    Require(stats.admission_timeouts == 1 && stats.pending_admissions == 0 &&
        stats.reconnecting_participants == 0 && service->ParticipantCount() == 1,
        "admitted connection expired or recovery state leaked");
    Require(stats.join_attempts == 2 && stats.join_succeeded == 1 && stats.join_failed == 1 &&
        stats.resume_attempts == 2 && stats.resume_succeeded == 1 && stats.resume_failed == 1 &&
        stats.admission_validation_failed == 1 && stats.admission_binding_failed == 1 &&
        stats.join_duration_us_total >= stats.join_duration_us_max && stats.join_duration_us_max > 0 &&
        stats.resume_duration_us_total >= stats.resume_duration_us_max && stats.resume_duration_us_max > 0,
        "admission outcome or duration stats incorrect");
    Require(recovered.Request({{"type", "leave"}}).value("type", "") == "closed", "leave failed");
    recovered.Send(1, Json{{"type", "join"}, {"room_id", "lifecycle"},
        {"token", RoomToken("transient", "lifecycle")}}.dump());
    recovered.Send(1, "{");
    recovered.Send(1, Json{{"type", "tracks"}}.dump());
    recovered.Send(1, Json{{"type", "leave"}}.dump());
    recovered.Send(1, Json{{"type", "join"}, {"room_id", "lifecycle"},
        {"token", RoomToken("replacement", "lifecycle")}}.dump());
    Require(recovered.Receive().value("type", "") == "joined" &&
        recovered.Receive().value("type", "") == "error" && recovered.Receive().value("type", "") == "tracks" &&
        recovered.Receive().value("type", "") == "closed" && recovered.Receive().value("type", "") == "joined",
        "pipelined join/leave/join responses reordered");
    std::this_thread::sleep_for(650ms);
    Require(service->GetStats().pending_admissions == 0 && service->ParticipantCount() == 1,
        "old initial response acknowledgement changed new admission deadline");
    recovered.Close();
    wait([](const auto& state) { return state.recovery_expired == 1; });
    Require(service->ParticipantCount() == 0, "expired recovery retained participant");
    service->Stop();
    stats = service->GetStats();
    Require(stats.signaling_opened == 3 && stats.signaling_closed == 3 && stats.active_signaling == 0 &&
        stats.pending_admissions == 0 && stats.reconnecting_participants == 0,
        "stop did not preserve counters and clear gauges");
    loop.Stop();
}

void RunCoordinatorChannels()
{
    using namespace server::signaling;
    namespace auth = service::auth;
    EventLoop loop(1);
    Require(loop.Start(), "coordinator loop did not start");
    auto owner = loop.GetTaskScheduler();
    config::AppConfig config;
    config.listen_ip = config.webrtc.public_ip = "127.0.0.1";
    config.udp_port = 0;
    config.webrtc.reconnect_timeout_ms = 1000;
    auto authService = std::make_shared<auth::AuthService>(
        std::make_shared<auth::JwtAuthenticator>(std::make_shared<auth::StaticKeyProvider>(
            std::map<std::string, std::string>{{kApiKey, kApiSecret}})),
        std::make_shared<auth::RoomAccessPolicy>());
    server::WebRtcServiceStats stats;
    auto room = std::make_shared<RoomSignalHandler>(owner, config, nullptr, authService, stats);
    auto coordinator = std::make_shared<SignalSessionCoordinator>(owner, room, AdmissionValidator(authService, {}), stats, 50);
    std::deque<std::function<void()>> outgoing;
    std::vector<SignalResponse> received;
    const auto open = [&](SignalKey key, bool acknowledge = true, bool writable = true)
    {
        std::shared_ptr<SignalSession> session;
        owner->Invoke([&] {
            session = coordinator->Open(key,
                [&, writable](auto task) {
                    if (!writable) return false;
                    outgoing.push_back(std::move(task)); return true;
                },
                [&, key, acknowledge](SignalResponse response) {
                    if (acknowledge && response.initial) coordinator->ConfirmInitial(key, response.admission_generation);
                    received.push_back(std::move(response));
                });
        });
        Require(session != nullptr, "coordinator open failed");
        return session;
    };
    const auto send = [&](const auto& session, Json body) {
        SignalRequest request; Json error;
        const auto bytes = body.dump();
        Require(DecodeRequest(bytes, request, error), "request decode failed");
        Require(session->Request(std::move(request), bytes.size()) == SignalResult::Accepted, "request rejected");
        owner->Invoke([] {});
    };
    const auto join = [&](const auto& session, const std::string& identity) {
        send(session, {{"type", "join"}, {"room_id", "channels"}, {"token", RoomToken(identity, "channels")}});
    };
    const auto pump = [&] {
        owner->Invoke([&] {
            while (!outgoing.empty())
            { auto task = std::move(outgoing.front()); outgoing.pop_front(); task(); }
        });
    };
    const auto cleanup = [&] {
        owner->Invoke([&] { coordinator->Stop(); room->Stop(); });
        loop.Stop();
    };
    try
    {
        owner->Invoke([&] { Require(room->Start(), "coordinator room start failed"); coordinator->Start(); });
        // 响应执行器拒收：新入房参与者必须回滚，不能变成可恢复的孤儿。
        auto failed = open({"same-id", 1}, true, false);
        join(failed, "failed");
        Require(failed->IsCancelled(), "failed response post did not cancel channels");
        owner->Invoke([&] {
            Require(room->ParticipantCount() == 1, "test did not reach committed join");
            coordinator->Closed(failed->Key());
            Require(room->ParticipantCount() == 0, "failed initial response leaked participant");
        });
        auto current = open({"same-id", 2});
        join(current, "reusable"); pump();
        Require(received.back().body.value("type", "") == "joined", "new generation did not join");
        const auto joined = received.back().body;
        owner->Invoke([&] {
            coordinator->Closed(failed->Key()); coordinator->ConfirmInitial(failed->Key(), 1);
            Require(room->HasMember("same-id") && coordinator->GetStats().active_signaling == 1,
                "stale close affected new generation");
            coordinator->Closed(current->Key());
        });
        // 恢复后的首响失败：只撤销本次信令绑定，保留既有参与者。
        auto resumed = open({"resume-1", 3}, false);
        Json resume = {{"type", "resume"}, {"room_id", "channels"}, {"token", RoomToken("reusable", "channels")},
            {"participant_id", joined.at("participant_id")}, {"resume_token", joined.at("resume_token")}};
        send(resumed, resume);
        owner->Invoke([&] {
            coordinator->Closed(resumed->Key()); coordinator->Closed(resumed->Key());
            Require(room->ParticipantCount() == 1 && room->ReconnectingCount() == 1,
                "failed resume removed existing participant");
        });
        auto recovered = open({"resume-2", 4});
        send(recovered, resume); pump();
        Require(received.back().body.value("type", "") == "resumed", "retry resume failed");
        // 房间已提交而首响尚未交给连接队列时，期限到达也要回滚新资源。
        auto delayed = open({"delayed", 5}, false);
        join(delayed, "delayed");
        std::this_thread::sleep_for(65ms);
        owner->Invoke([&] {
            Require(!coordinator->ConfirmInitial(delayed->Key(), 1),
                "late initial acknowledgement bypassed admission deadline");
            coordinator->Tick();
            Require(delayed->IsCancelled() && !room->HasMember("delayed"), "initial response timeout leaked join");
            Require(room->HasMember("resume-2"), "initial timeout affected another connection");
            coordinator->Closed(delayed->Key());
        });
        pump();
        send(recovered, {{"type", "leave"}}); pump();
        owner->Invoke([&] { coordinator->Closed(recovered->Key()); });
        // 房间的真实 ICE 到期通知经响应通道发送，不依赖新的客户端请求。
        received.clear();
        auto active = open({"notify", 6});
        join(active, "notify");
        send(active, {{"type", "publish"}, {"sdp", Offer()}});
        pump();
        Require(received.size() == 2 && received[0].initial && received[1].body.value("type", "") == "answer",
            "initial response did not precede pipelined publication");
        owner->Invoke([&] { coordinator->Tick(); });
        pump();
        Require(received.size() == 3 && received[2].body.value("type", "") == "tracks_changed" &&
            received[2].body.at("tracks").size() == 2,
            "two track publications were not coalesced after initial and answer responses");
        owner->Invoke([&] { coordinator->Tick(); });
        pump();
        Require(received.size() == 3, "unchanged track revision emitted duplicate notifications");
        // 使用真实 ICE 计时器，避免把推进业务 Tick 的参数误当作媒体时钟。
        std::this_thread::sleep_for(30100ms);
        owner->Invoke([&] { coordinator->Tick(); });
        pump();
        Require(received.size() == 4 && received[0].body.value("type", "") == "joined" &&
            received[1].body.value("type", "") == "answer" && received[3].close_after,
            "room notification bypassed initial response or response channel");
        owner->Invoke([&] {
            coordinator->Closed(active->Key());
            Require(room->ParticipantCount() == 0, "room termination left participant");
        });
        // 停服取消尚未执行的请求及响应，排队任务随后运行也不能重新创建资源。
        auto stopped = open({"stopped", 7});
        owner->Invoke([&] {
            stopped->Request({SignalRequest::Type::Join, {{"type", "join"}, {"room_id", "channels"},
                {"token", RoomToken("stopped", "channels")}}}, 100);
            coordinator->Stop(); room->Stop();
        });
        owner->Invoke([] {}); pump();
        Require(stopped->IsCancelled(), "stop did not cancel channels");
        cleanup();
    }
    catch (...) { cleanup(); throw; }
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
    a.Send(1, Json{{"type", "publish"}, {"sdp", Offer()}}.dump());
    const auto publication = a.ReceiveFrame();
    CheckAnswer(publication, config.udp_port);
    const auto changed = b.ReceiveTrackChange();
    Require(changed.at("room_id") == "demo" && changed.at("tracks").size() == 2 &&
        changed.at("revision").get<uint64_t>() > 0, "publication did not notify idle room peer");
    const auto source = std::dynamic_pointer_cast<media::SfuEndpoint>(utils::EndpointManager::Instance().Find(
        publication.at("session_id").get<uint64_t>()));
    Require(source && source->PublishedTrackCount() == 2, "room source endpoint missing");
    const auto list = [&](Client& client) {
        return client.Request({{"type", "tracks"}}).at("tracks");
    };
    const auto tracks = list(b);
    Require(tracks.size() == 2 && list(outsider).empty(), "room track visibility incorrect");
    Require(outsider.PendingTrackChanges() == 0, "track event leaked to another room");
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
    const auto secondChanged = b.ReceiveTrackChange();
    Require(secondChanged.at("tracks").size() == 4 && secondChanged.at("revision") > changed.at("revision"),
        "second publication did not update room snapshot");
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
    const auto recoveredStream = media::MediaStreamRegistry::Instance().FindByName(publisherId, "demo");
    Require(recoveredStream && recoveredStream->GetState() == media::MediaStream::State::Publishing,
        "signaling recovery lost the core publication");
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
    Require(media::MediaStreamRegistry::Instance().FindByName(publisherId, "demo") == recoveredStream,
        "ICE restart replaced the core publication");
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
    const auto recoveredTracks = resumedViewer.ReceiveTrackChange();
    Require(recoveredTracks.at("tracks").size() == 4 && recoveredTracks.at("revision") == secondChanged.at("revision"),
        "new signaling generation did not receive current tracks after resumed response");
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
    const auto removed = resumedViewer.ReceiveTrackChange();
    Require(removed.at("tracks").size() == 2 && removed.at("revision") > secondChanged.at("revision"),
        "recovery expiry did not notify track removal");
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (list(resumedViewer).size() != 2)
    {
        Require(std::chrono::steady_clock::now() < deadline, "publisher disconnect left stale room tracks");
        std::this_thread::sleep_for(5ms);
    }
    Require(destination->SubscriptionCount() == 0, "publisher disconnect retained downstream routes");
    Require(!media::MediaStreamRegistry::Instance().FindById(recoveredStream->GetId()),
        "recovery expiry retained the core publication");
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

void RunMembers()
{
    // Bus 发布的是已提交的值快照；成员更新、状态和移除共享一个版本序列。
    auto coreRoom = std::make_shared<room::Room>(room::RoomInfo{"member-events", "events"});
    std::vector<uint64_t> versions;
    bool removedEvent = false;
    bool privateBinding = false;
    auto observation = coreRoom->Events().Subscribe<room::ParticipantChangedEvent>([&](const auto& event) {
        versions.push_back(event.revision);
        removedEvent = event.removed;
        privateBinding = privateBinding || !event.participant.signaling.connection_id.empty();
    });
    auto coreMember = std::make_shared<room::Participant>("member-1", "Original");
    Require(coreMember->BindSignaling("private-connection"), "core member signaling bind failed");
    Require(coreRoom->Join(coreMember), "core member join failed");
    Require(coreRoom->UpdateParticipant("member-1", "Updated", {"metadata", {{"key", "value"}}}, {}),
        "core member update failed");
    Require(coreRoom->Leave("member-1") && removedEvent && !privateBinding && versions == std::vector<uint64_t>{1, 2, 3},
        "member bus did not publish committed lifecycle revisions");

    const auto baseline = utils::EndpointManager::Instance().Size();
    EventLoop loop(3);
    Require(loop.Start(), "member loop did not start");
    config::AppConfig config;
    config.listen_ip = config.webrtc.public_ip = "127.0.0.1";
    config.udp_port = AvailablePort(SOCK_DGRAM);
    config.websocket_port = AvailablePort(SOCK_STREAM);
    config.webrtc.enabled = true;
    config.webrtc.api_keys = {{kApiKey, kApiSecret}};
    config.webrtc.max_sessions = 8;
    config.webrtc.reconnect_timeout_ms = 5000;
    auto service = std::make_shared<server::WebRtcService>(&loop, config, nullptr);
    Require(service->Start(), "member service did not start");
    Client admin(config.websocket_port), user(config.websocket_port), observer(config.websocket_port), outsider(config.websocket_port);
    for (const auto* type : {"participants", "participant", "update_participant", "kick"})
        Require(observer.Request({{"type", type}}).value("code", "") == "unauthenticated", "member command bypassed admission");
    const auto adminToken = RoomToken("owner", "members", true, true, 0, kApiKey, kApiSecret, true, true);
    admin.Send(1, Json{{"type", "join"}, {"room_id", "members"}, {"token", adminToken},
        {"name", "Owner"}, {"metadata", "room creator"}, {"attributes", {{"team", "media"}}}}.dump());
    const auto ownerJoined = admin.ReceiveFrame();
    Require(ownerJoined.value("type", "") == "joined" && ownerJoined.at("participant").at("name") == "Owner",
        "initial response did not precede member notifications or lost admission profile");
    const auto adminId = ownerJoined.at("participant_id");
    const auto userToken = RoomToken("member", "members", true, true, 0, kApiKey, kApiSecret, false, true);
    const auto userJoined = user.Request({{"type", "join"}, {"room_id", "members"}, {"token", userToken}});
    Require(userJoined.value("type", "") == "joined", "member admission failed");
    const auto userId = userJoined.at("participant_id");
    Require(observer.Request({{"type", "join"}, {"room_id", "members"},
        {"token", RoomToken("reader", "members")}, {"name", "untrusted"}}).value("code", "") == "forbidden",
        "admission profile bypassed self-update grant");
    const auto readerJoined = Join(observer, "reader", "members", false, false);
    const auto outsiderJoined = Join(outsider, "outsider", "elsewhere");
    const auto changed = admin.ReceiveMemberChange();
    Require(changed.at("room_id") == "members" && changed.contains("revision") && !changed.contains("participants"),
        "member changes did not use bounded invalidation notifications");

    const auto query = [&](const Json& id) { return admin.Request({{"type", "participant"}, {"participant_id", id}}); };
    const auto roster = admin.Request({{"type", "participants"}});
    Require(roster.at("total") == 3 && roster.at("participants").size() == 3, "room roster leaked another room or failed admission");
    for (const auto& item : roster.at("participants"))
        Require(!item.contains("resume_token") && !item.contains("signaling") && !item.contains("connection_id"),
            "roster exposed private binding or recovery credentials");
    const auto first = admin.Request({{"type", "participants"}, {"limit", 1}});
    const auto second = admin.Request({{"type", "participants"}, {"limit", 1}, {"cursor", first.at("next_cursor")},
        {"revision", first.at("revision")}});
    Require(first.at("participants").size() == 1 && second.at("participants").size() == 1 &&
        first.at("participants")[0].at("participant_id") < second.at("participants")[0].at("participant_id"),
        "stable roster pagination repeated a participant");
    Require(admin.Request({{"type", "participants"}, {"cursor", first.at("next_cursor")}}).value("type", "") == "error",
        "pagination accepted unversioned continuation");
    for (const auto& limit : Json::array({0, -1, 101, 1.5, "1", nullptr, 18446744073709551615ull}))
        Require(admin.Request({{"type", "participants"}, {"limit", limit}}).value("type", "") == "error", "invalid roster limit accepted");
    Require(query(outsiderJoined.at("participant_id")).value("type", "") == "error", "single member query crossed rooms");
    Require(admin.Request({{"type", "kick"}, {"participant_id", outsiderJoined.at("participant_id")}}).value("type", "") == "error",
        "administrator kicked a different room's participant");
    Require(user.Request({{"type", "update_participant"}, {"participant_id", adminId}, {"name", "attacker"}})
        .value("code", "") == "forbidden", "member edited another participant");
    Require(user.Request({{"type", "kick"}, {"participant_id", adminId}}).value("code", "") == "forbidden", "member kicked administrator");
    Require(user.Request({{"type", "update_participant"}, {"permissions", {{"room_admin", true}}}})
        .value("code", "") == "forbidden", "member elevated own privileges");
    Require(observer.Request({{"type", "update_participant"}, {"name", "not permitted"}}).value("code", "") == "forbidden",
        "missing self-update grant was ignored");

    const auto original = query(userId);
    Require(user.Request({{"type", "update_participant"}, {"name", "partial"}, {"attributes", {{"bad", 7}}}})
        .value("type", "") == "error", "invalid attribute type accepted");
    Require(admin.Request({{"type", "update_participant"}, {"participant_id", userId}, {"name", "partial"},
        {"permissions", {{"can_publish", false}, {"unknown", true}}}}).value("type", "") == "error", "unknown permission accepted");
    Require(user.Request({{"type", "update_participant"}, {"metadata", std::string(4097, 'x')}}).value("type", "") == "error",
        "oversize metadata accepted");
    Require(query(userId) == original, "failed participant patch partially committed data or revision");
    // 资料可包含 JSON 转义字符，分页按编码后字节数收敛，而不是只按人数限制。
    Json attributes = Json::object();
    for (int i = 0; i < 16; ++i) attributes["key" + std::to_string(i)] = std::string(500, '\x01');
    for (const auto& id : {adminId, userId, readerJoined.at("participant_id")})
        Require(admin.Request({{"type", "update_participant"}, {"participant_id", id},
            {"metadata", std::string(4096, '\x01')}, {"attributes", attributes}}).value("type", "") == "participant_updated",
            "bounded escaped participant profile rejected");
    // 六位转义会让三人的资料超过页面预算，仍须可通过游标读完。
    const auto bounded = admin.Request({{"type", "participants"}, {"limit", 100}});
    Require(bounded.dump().size() <= server::signaling::kMaxMessageBytes &&
        bounded.at("participants").size() < 3 && !bounded.at("next_cursor").get<std::string>().empty(),
        "roster encoding exceeded bounded response budget");
    Json deletions = Json::object();
    for (auto it = attributes.begin(); it != attributes.end(); ++it) deletions[it.key()] = nullptr;
    for (const auto& id : {adminId, userId, readerJoined.at("participant_id")})
        Require(admin.Request({{"type", "update_participant"}, {"participant_id", id},
            {"metadata", ""}, {"attributes", deletions}}).value("type", "") == "participant_updated", "profile cleanup failed");
    const auto updated = user.Request({{"type", "update_participant"}, {"name", "Alice"}, {"metadata", "profile-v1"},
        {"attributes", {{"team", "audio"}, {"remove_me", "yes"}}}});
    Require(updated.value("type", "") == "participant_updated" && updated.at("participant").at("name") == "Alice",
        "self profile update failed");
    const auto deleted = user.Request({{"type", "update_participant"}, {"attributes", {{"remove_me", nullptr}, {"empty", ""}}}});
    Require(!deleted.at("participant").at("attributes").contains("remove_me") &&
        deleted.at("participant").at("attributes").at("empty") == "" && deleted.at("participant").at("metadata") == "profile-v1",
        "attribute merge/delete or omitted field preservation failed");
    Require(admin.Request({{"type", "participants"}, {"cursor", first.at("next_cursor")}, {"revision", first.at("revision")}})
        .value("code", "") == "snapshot_changed", "pagination mixed roster versions");

    const auto publication = user.Request({{"type", "publish"}, {"sdp", Offer()}});
    CheckAnswer(publication, config.udp_port);
    const auto publishedId = publication.at("session_id").get<uint64_t>();
    const auto revoked = admin.Request({{"type", "update_participant"}, {"participant_id", userId},
        {"permissions", {{"can_publish", false}, {"can_update_own_metadata", false}}}});
    Require(revoked.at("media_stopped") == true && !utils::EndpointManager::Instance().Find(publishedId),
        "publication continued after permission revocation");
    Require(user.ReceivePermissionsChange().at("media_stopped") == true, "permission revocation notification missing");
    Require(user.Request({{"type", "publish"}, {"sdp", Offer()}}).value("code", "") == "forbidden",
        "revoked publish permission allowed a new session");
    Require(user.Request({{"type", "update_participant"}, {"name", "revoked"}}).value("code", "") == "forbidden",
        "revoked self-update permission remained effective");
    const auto ownerPublication = admin.Request({{"type", "publish"}, {"sdp", Offer()}});
    CheckAnswer(ownerPublication, config.udp_port);
    auto browser = ParseDescription({{"sdp", Offer()}}, sdp::SdpType::Offer);
    Json bindings = Json::array();
    for (auto& media : browser.medias)
    {
        media.direction = sdp::MediaDirection::RecvOnly;
        media.ssrcs.clear(); media.ssrcGroups.clear();
        bindings.push_back({{"track_id", adminId.get<std::string>() + ":" + media.mid}, {"mid", media.mid}});
    }
    const auto subscription = user.Request({{"type", "subscribe"}, {"sdp", sdp::Sdp::Serialize(browser)}, {"tracks", bindings}});
    Require(subscription.value("type", "") == "answer", "member subscription failed");
    const auto subscribedId = subscription.at("session_id").get<uint64_t>();
    Require(admin.Request({{"type", "update_participant"}, {"participant_id", userId},
        {"permissions", {{"can_subscribe", false}}}}).at("media_stopped") == true &&
        !utils::EndpointManager::Instance().Find(subscribedId), "subscription continued after permission revocation");
    Require(user.ReceivePermissionsChange().at("media_stopped") == true, "subscription revoke notification missing");

    const auto waitReconnecting = [&](std::size_t expected) {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (service->GetStats().reconnecting_participants != expected && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(10ms);
        Require(service->GetStats().reconnecting_participants == expected, "disconnect did not enter recovery state");
    };
    user.Close(); waitReconnecting(1);
    Require(query(userId).at("participant").at("state") == "reconnecting", "member query lost recovery state");
    Client resumed(config.websocket_port);
    const Json resume = {{"type", "resume"}, {"room_id", "members"}, {"token", userToken},
        {"participant_id", userId}, {"resume_token", userJoined.at("resume_token")}};
    const auto recovered = resumed.Request(resume);
    Require(recovered.value("type", "") == "resumed" && !recovered.at("participant").at("permissions").at("can_publish").get<bool>() &&
        recovered.at("participant").at("name") == "Alice" && recovered.at("ice_restart") == false,
        "resume restored stale token grants or discarded profile");
    Require(resumed.Request({{"type", "subscribe"}, {"sdp", Offer()}, {"tracks", bindings}}).value("code", "") == "forbidden",
        "resume restored revoked subscription permission");
    Require(admin.Request({{"type", "remove_participant"}, {"participant_id", userId}}).value("type", "") == "participant_removed",
        "administrator kick failed");
    Require(resumed.Receive().value("type", "") == "removed", "kicked client did not receive terminal notification");
    Client kickedRecovery(config.websocket_port);
    Require(kickedRecovery.Request(resume).value("type", "") == "error", "kick left usable recovery credentials");
    // 断线保留的成员也可被移除，旧连接清理不能影响后来重新入会的同身份成员。
    observer.Close(); waitReconnecting(1);
    Require(admin.Request({{"type", "kick"}, {"participant_id", readerJoined.at("participant_id")}}).value("type", "") == "participant_removed",
        "administrator could not remove reconnecting participant");
    const auto rejoined = Join(kickedRecovery, "member", "members");
    const auto kickedMedia = kickedRecovery.Request({{"type", "publish"}, {"sdp", Offer()}});
    Require(kickedMedia.value("type", "") == "answer", "fresh member could not publish after previous removal");
    Require(admin.Request({{"type", "kick"}, {"participant_id", rejoined.at("participant_id")}}).value("type", "") == "participant_removed" &&
        !utils::EndpointManager::Instance().Find(kickedMedia.at("session_id").get<uint64_t>()), "kick retained active media endpoint");
    Require(kickedRecovery.Receive().value("type", "") == "removed", "active publisher did not receive kick notification");
    Client voluntary(config.websocket_port);
    Join(voluntary, "voluntary", "members");
    Require(voluntary.Request({{"type", "leave"}}).value("type", "") == "closed", "member leave failed");
    Require(admin.Request({{"type", "participants"}}).at("total") == 1 && query(userId).value("type", "") == "error",
        "kick or leave retained room membership");
    Require(admin.Request({{"type", "leave"}}).value("type", "") == "closed", "owner leave failed");
    outsider.Request({{"type", "leave"}});
    WaitForEndpoints(baseline);
    service->Stop(); loop.Stop();
}

void RunMediaControls()
{
    const auto baseline = utils::EndpointManager::Instance().Size();
    auto& streams = media::MediaStreamRegistry::Instance();
    const auto streamBaseline = streams.Size();
    EventLoop loop(2);
    Require(loop.Start(), "media controls event loop did not start");
    config::AppConfig config;
    config.listen_ip = config.webrtc.public_ip = "127.0.0.1";
    config.udp_port = AvailablePort(SOCK_DGRAM);
    config.websocket_port = AvailablePort(SOCK_STREAM);
    config.webrtc.enabled = true;
    config.webrtc.api_keys = {{kApiKey, kApiSecret}};
    config.webrtc.max_sessions = 3;
    config.webrtc.reconnect_timeout_ms = 1200;
    auto service = std::make_shared<server::WebRtcService>(&loop, config, nullptr);
    Require(service->Start(), "media controls service did not start");
    Client a(config.websocket_port), b(config.websocket_port), observer(config.websocket_port);
    Require(observer.Request({{"type", "unpublish"}, {"session_id", 1}}).value("code", "") == "unauthenticated",
        "media stop bypassed admission");
    const auto joined = Join(a, "controls-publisher", "controls");
    const auto publisherId = joined.at("participant_id").get<std::string>();
    Join(b, "controls-viewer", "controls");
    Join(observer, "controls-observer", "controls", false, false);
    Require(streams.Size() == streamBaseline, "joining registered an unpublished media stream");
    for (const auto* type : {"unpublish", "unsubscribe"})
        Require(observer.Request({{"type", type}, {"session_id", 1}}).value("code", "") == "forbidden",
            "media stop bypassed action permission");

    const auto publication = a.Request({{"type", "publish"}, {"sdp", Offer()}});
    CheckAnswer(publication, config.udp_port);
    const auto sourceId = publication.at("session_id").get<uint64_t>();
    auto source = std::dynamic_pointer_cast<media::SfuEndpoint>(utils::EndpointManager::Instance().Find(sourceId));
    Require(source && source->PublishedTrackCount() == 2, "media controls publication missing");
    const auto core = streams.FindByName(publisherId, "controls");
    Require(core && streams.Size() == streamBaseline + 1 && core->GetSource().protocol == "webrtc" &&
        core->GetSource().publisher_id == publisherId && core->GetState() == media::MediaStream::State::Publishing,
        "WebRTC publication did not enter the shared stream registry");
    const auto coreTracks = core->GetSnapshot().tracks;
    Require(coreTracks.size() == 2, "core publication lost negotiated tracks");
    for (const auto& track : coreTracks)
    {
        const auto resolved = streams.FindTrack(track.ref);
        Require(resolved && resolved->endpoint_id == sourceId && track.description.Valid(),
            "core track reference did not resolve to the publishing endpoint");
    }
    const auto published = b.ReceiveTrackChange();
    Require(published.at("tracks").size() == 2, "media controls publish event missing");
    Require(a.Request({{"type", "unpublish"}}).value("type", "") == "error", "missing stop session_id accepted");
    const Json invalidIds = Json::array({nullptr, false, 0, -1, 1.0, "1", Json::object(), Json::array()});
    for (const auto& id : invalidIds)
        Require(a.Request({{"type", "unpublish"}, {"session_id", id}}).value("type", "") == "error",
            "malformed stop session_id accepted");
    Require(a.RequestText("{\"type\":\"unpublish\",\"session_id\":18446744073709551616}").value("type", "") == "error",
        "overflowing stop session_id accepted");
    Require(a.Request({{"type", "unsubscribe"}, {"session_id", sourceId}}).value("type", "") == "error",
        "unsubscribe stopped a publishing session");
    Require(utils::EndpointManager::Instance().Find(sourceId) == source && source->PublishedTrackCount() == 2,
        "invalid stop request removed publication");

    auto browser = ParseDescription({{"sdp", Offer()}}, sdp::SdpType::Offer);
    Json bindings = Json::array();
    for (auto& media : browser.medias)
    {
        media.direction = sdp::MediaDirection::RecvOnly;
        media.ssrcs.clear(); media.ssrcGroups.clear();
        bindings.push_back({{"track_id", publisherId + ":" + media.mid}, {"mid", media.mid}});
    }
    const Json subscription = {{"type", "subscribe"}, {"sdp", sdp::Sdp::Serialize(browser)}, {"tracks", bindings}};
    const auto subscribe = [&] {
        const auto reply = b.Request(subscription);
        Require(reply.value("type", "") == "answer", reply.dump().c_str());
        const auto id = reply.at("session_id").get<uint64_t>();
        auto endpoint = std::dynamic_pointer_cast<media::SfuEndpoint>(utils::EndpointManager::Instance().Find(id));
        Require(endpoint && endpoint->SubscriptionCount() == 2, "resubscribe did not bind forwarding routes");
        return endpoint;
    };
    auto destination = subscribe();
    Require(streams.Size() == streamBaseline + 1, "subscription registered a duplicate source stream");
    for (const auto& track : coreTracks)
    {
        const auto resolved = streams.FindTrack(track.ref);
        Require(resolved && resolved->description.ref == track.ref && resolved->consumers.size() == 1,
            "room RTP subscription did not reference the global source track");
    }
    const auto oldDestinationId = destination->Id();
    Require(b.Request({{"type", "unpublish"}, {"session_id", oldDestinationId}}).value("type", "") == "error",
        "unpublish stopped a subscribing session");
    Require(b.Request({{"type", "unsubscribe"}, {"session_id", sourceId}}).value("type", "") == "error" &&
        destination->SubscriptionCount() == 2, "another participant's session_id stopped local media");
    const Json stopWatching = {{"type", "unsubscribe"}, {"session_id", oldDestinationId}};
    const auto stoppedViewer = b.Request(stopWatching);
    Require(stoppedViewer.value("type", "") == "unsubscribed" && stoppedViewer.at("changed") == true &&
        stoppedViewer.at("session_id") == oldDestinationId, "unsubscribe acknowledgement incorrect");
    Require(!utils::EndpointManager::Instance().Find(oldDestinationId) && destination->SubscriptionCount() == 0 &&
        source->PublishedTrackCount() == 2, "unsubscribe did not isolate downstream cleanup");
    Require(b.Request(stopWatching).at("changed") == false, "repeated unsubscribe was not idempotent");
    destination = subscribe();
    Require(destination->Id() != oldDestinationId && b.Request(stopWatching).value("type", "") == "error" &&
        destination->SubscriptionCount() == 2, "stale unsubscribe removed replacement media");

    const Json stopPublishing = {{"type", "unpublish"}, {"session_id", sourceId}};
    const auto stoppedPublisher = a.Request(stopPublishing);
    Require(stoppedPublisher.value("type", "") == "unpublished" && stoppedPublisher.at("changed") == true &&
        stoppedPublisher.at("session_id") == sourceId, "unpublish acknowledgement incorrect");
    Require(!utils::EndpointManager::Instance().Find(sourceId) && destination->SubscriptionCount() == 0,
        "unpublish retained source endpoint or downstream routes");
    Require(!streams.FindById(core->GetId()) && !streams.FindTrack(coreTracks.front().ref) &&
        core->GetState() == media::MediaStream::State::Closed && streams.Size() == streamBaseline,
        "unpublish retained the old core publication");
    const auto unpublished = b.ReceiveTrackChange();
    Require(unpublished.at("tracks").empty() && unpublished.at("revision") > published.at("revision"),
        "unpublish did not notify track removal");
    Require(a.Request(stopPublishing).at("changed") == false, "repeated unpublish was not idempotent");
    const auto beforeFailure = utils::EndpointManager::Instance().Size();
    Require(b.Request({{"type", "unsubscribe"}, {"session_id", destination->Id()}}).at("changed") == true,
        "viewer could not stop after source disappeared");
    Require(b.Request(subscription).value("type", "") == "error" &&
        utils::EndpointManager::Instance().Size() + 1 == beforeFailure, "unavailable source left a partial subscriber");

    const auto replacement = a.Request({{"type", "publish"}, {"sdp", Offer()}});
    CheckAnswer(replacement, config.udp_port);
    const auto replacementId = replacement.at("session_id").get<uint64_t>();
    const auto nextCore = streams.FindByName(publisherId, "controls");
    Require(nextCore && nextCore->GetId() != core->GetId() && !streams.Remove(core) &&
        streams.FindByName(publisherId, "controls") == nextCore, "stale cleanup removed the replacement core stream");
    Require(replacementId != sourceId && a.Request(stopPublishing).value("type", "") == "error" &&
        utils::EndpointManager::Instance().Find(replacementId), "stale unpublish removed replacement media");
    const auto republished = b.ReceiveTrackChange();
    Require(republished.at("tracks").size() == 2 && republished.at("revision") > unpublished.at("revision"),
        "republish did not notify room peers");
    for (const auto& track : republished.at("tracks"))
        Require(track.at("publisher_id") == publisherId, "republish changed participant identity");
    destination = subscribe();
    const auto removedTrack = nextCore->GetTracks().front().ref;
    Require(streams.RemoveTrack(removedTrack), "global track removal failed");
    const auto changed = b.ReceiveTrackChange();
    Require(changed.at("tracks").size() == 1 && destination->SubscriptionCount() == 1,
        "global removal did not reconcile room notification and downstream route");
    auto replacementEndpoint = std::dynamic_pointer_cast<media::SfuEndpoint>(
        utils::EndpointManager::Instance().Find(replacementId));
    Require(replacementEndpoint && replacementEndpoint->PublishedTrackCount() == 1 &&
        nextCore->GetTracks().size() == 1, "global removal did not release the source receiver on its owner");
    Require(a.Request({{"type", "unpublish"}, {"session_id", replacementId}}).at("changed") == true,
        "second publication could not stop");
    Require(b.Request({{"type", "unsubscribe"}, {"session_id", destination->Id()}}).at("changed") == true,
        "second subscription could not stop");
    Require(utils::EndpointManager::Instance().Size() == baseline, "media stops leaked endpoints");

    // 停止媒体后仍可用原身份和凭据恢复信令，无需 ICE restart。
    a.Close();
    std::this_thread::sleep_for(50ms);
    Client resumed(config.websocket_port);
    const auto recovery = resumed.Request({{"type", "resume"}, {"token", RoomToken("controls-publisher", "controls")},
        {"room_id", "controls"}, {"participant_id", publisherId}, {"resume_token", joined.at("resume_token")}});
    Require(recovery.value("type", "") == "resumed" && recovery.at("participant_id") == publisherId &&
        recovery.at("ice_restart") == false, "media stop removed recoverable membership");
    const auto resumedPublication = resumed.Request({{"type", "publish"}, {"sdp", Offer()}});
    CheckAnswer(resumedPublication, config.udp_port);
    const auto resumedId = resumedPublication.at("session_id").get<uint64_t>();
    const auto resumedCore = streams.FindByName(publisherId, "controls");
    Require(resumedCore && streams.Remove(resumedCore), "global stream removal failed");
    for (unsigned i = 0; i < 200 && utils::EndpointManager::Instance().Find(resumedId); ++i)
        std::this_thread::sleep_for(10ms);
    Require(!utils::EndpointManager::Instance().Find(resumedId) &&
        resumed.Request({{"type", "unpublish"}, {"session_id", resumedId}}).at("changed") == false,
        "global stream removal did not clean the owner's media session");
    CheckAnswer(resumed.Request({{"type", "publish"}, {"sdp", Offer()}}), config.udp_port);
    for (auto* client : {&resumed, &b, &observer})
        Require(client->Request({{"type", "leave"}}).value("type", "") == "closed", "leave after media stop failed");
    WaitForEndpoints(baseline);
    service->Stop(); loop.Stop();
    Require(streams.Size() == streamBaseline, "media controls leaked core streams");
}

void RunRoomOwners()
{
    using server::signaling::SignalSessionCoordinator;
    const auto baseline = utils::EndpointManager::Instance().Size();
    EventLoop loop(3);
    Require(loop.Start(), "room owners loop did not start");
    const auto executors = loop.GetTaskSchedulers();
    config::AppConfig config;
    config.listen_ip = config.webrtc.public_ip = "127.0.0.1";
    config.udp_port = AvailablePort(SOCK_DGRAM);
    config.websocket_port = AvailablePort(SOCK_STREAM);
    config.webrtc.enabled = true;
    config.webrtc.api_keys = {{kApiKey, kApiSecret}};
    config.webrtc.max_sessions = 3;
    config.webrtc.reconnect_timeout_ms = 100;
    auto service = std::make_shared<server::WebRtcService>(&loop, config, nullptr);
    Require(service->Start(), "room owners service did not start");
    std::string firstRoom, secondRoom;
    for (int i = 0; firstRoom.empty() || secondRoom.empty(); ++i)
    {
        const auto name = "owner-" + std::to_string(i);
        (SignalSessionCoordinator::SelectOwner(name, 2) == 0 ? firstRoom : secondRoom) = name;
    }
    Client a(config.websocket_port), peer(config.websocket_port), b(config.websocket_port), extra(config.websocket_port);
    Join(a, "owner-a", firstRoom); Join(peer, "owner-peer", firstRoom); Join(b, "owner-b", secondRoom);
    Require(extra.Request({{"type", "join"}, {"room_id", secondRoom}, {"token", RoomToken("extra", secondRoom)}})
        .value("type", "") == "error", "room sharding multiplied the participant limit");
    const auto first = a.Request({{"type", "publish"}, {"sdp", Offer()}});
    const auto second = b.Request({{"type", "publish"}, {"sdp", Offer()}});
    CheckAnswer(first, config.udp_port); CheckAnswer(second, config.udp_port);
    Require(peer.ReceiveTrackChange().at("tracks").size() == 2, "same-owner publication notification missing");
    const auto checkIce = [&](const Json& answer) {
        const auto description = ParseDescription(answer, sdp::SdpType::Answer);
        const auto ice = description.medias.front().ice;
        protocol::IceRequestParams request;
        request.username = ice.ufrag + ":peer";
        request.password = ice.pwd;
        request.controlling = true; request.tie_breaker = 42; request.priority = 1234;
        std::vector<uint8_t> bytes(1500);
        size_t length = 0;
        Require(protocol::StunCodec::BuildIceBindingRequest(request, bytes.data(), bytes.size(), length), "ICE request build failed");
        Socket socket(SOCK_DGRAM);
        Require(socket.Bind(0), "ICE test socket bind failed");
        sockaddr_in target{};
        target.sin_family = AF_INET; target.sin_addr.s_addr = htonl(INADDR_LOOPBACK); target.sin_port = htons(config.udp_port);
        Require(::sendto(socket.Get(), bytes.data(), length, 0, reinterpret_cast<sockaddr*>(&target), sizeof(target)) ==
            static_cast<ssize_t>(length), "ICE request send failed");
        pollfd descriptor{socket.Get(), POLLIN, 0};
        Require(::poll(&descriptor, 1, 3000) > 0, "room owner did not return an ICE response through the shared UDP listener");
        const auto received = ::recv(socket.Get(), bytes.data(), bytes.size(), 0);
        protocol::StunMessageInfo response;
        Require(received > 0 && protocol::StunCodec::Parse(bytes.data(), received, response) && response.IsBindingResponse() &&
            protocol::StunCodec::VerifyMessageIntegrity(response, ice.pwd), "ICE response belonged to the wrong room session");
    };
    checkIce(first); checkIce(second);
    a.Request({{"type", "tracks"}}); peer.Request({{"type", "tracks"}});

    std::promise<void> entered, release;
    auto enteredFuture = entered.get_future();
    const auto gate = release.get_future().share();
    Require(executors[1]->Post([&entered, gate] { entered.set_value(); gate.wait_for(5s); }), "owner gate post failed");
    Require(enteredFuture.wait_for(3s) == std::future_status::ready, "owner gate did not start");
    a.Send(1, Json{{"type", "tracks"}}.dump());
    peer.Send(1, Json{{"type", "tracks"}}.dump());
    auto independent = std::async(std::launch::async, [&] { return b.Request({{"type", "tracks"}}); });
    const bool progressed = independent.wait_for(1500ms) == std::future_status::ready;
    const bool stayedOnOwner = !a.HasReadable() && !peer.HasReadable();
    release.set_value();
    const auto otherTracks = independent.get();
    Require(progressed && otherTracks.at("tracks").size() == 2, "blocked room owner stalled another owner or the network loop");
    Require(stayedOnOwner, "same-room state escaped its assigned owner");
    Require(a.Receive().at("tracks").size() == 2 && peer.Receive().at("tracks").size() == 2, "queued same-room requests were lost");

    // 同一连接离开后跨 owner 入房，必须先处理完上一条请求。
    a.Send(1, Json{{"type", "leave"}}.dump());
    a.Send(1, Json{{"type", "join"}, {"room_id", secondRoom}, {"token", RoomToken("moved", secondRoom)}}.dump());
    a.Send(1, Json{{"type", "tracks"}}.dump());
    Require(a.Receive().value("type", "") == "closed" && a.Receive().value("type", "") == "joined" &&
        a.Receive().at("tracks").size() == 2, "cross-owner leave/join pipeline reordered or retained the old room");
    Require(peer.Request({{"type", "tracks"}}).at("tracks").empty(), "room move retained the old publication");

    // 房间任务队列已满时，断连清理须在后续 Tick 重试，不能丢失。
    std::promise<void> saturatedEntered, saturatedRelease;
    auto saturatedFuture = saturatedEntered.get_future();
    const auto saturatedGate = saturatedRelease.get_future().share();
    executors[1]->Post([&saturatedEntered, saturatedGate] { saturatedEntered.set_value(); saturatedGate.wait_for(5s); });
    Require(saturatedFuture.wait_for(3s) == std::future_status::ready, "saturation gate did not start");
    std::size_t queued = 0;
    while (queued < 100000 && executors[1]->Post([] {})) ++queued;
    const auto closedBefore = service->GetStats().signaling_closed;
    peer.Close();
    const auto closedDeadline = std::chrono::steady_clock::now() + 2s;
    while (service->GetStats().signaling_closed == closedBefore && std::chrono::steady_clock::now() < closedDeadline)
        std::this_thread::sleep_for(2ms);
    const bool closedWhileFull = service->GetStats().signaling_closed == closedBefore + 1;
    saturatedRelease.set_value();
    Require(queued > 0 && queued < 100000 && closedWhileFull, "full room executor blocked or lost network close handling");
    const auto cleanupDeadline = std::chrono::steady_clock::now() + 3s;
    while (service->ParticipantCount() != 2 && std::chrono::steady_clock::now() < cleanupDeadline)
        std::this_thread::sleep_for(2ms);
    Require(service->ParticipantCount() == 2, "rejected room detach was not retried after queue recovery");
    Client pending(config.websocket_port);
    Join(pending, "owner-pending", firstRoom);

    std::promise<void> stoppingEntered, stopRelease;
    auto stoppingFuture = stoppingEntered.get_future();
    const auto stopGate = stopRelease.get_future().share();
    executors[1]->Post([&stoppingEntered, stopGate] { stoppingEntered.set_value(); stopGate.wait_for(5s); });
    Require(stoppingFuture.wait_for(3s) == std::future_status::ready, "stop gate did not start");
    pending.Send(1, Json{{"type", "publish"}, {"sdp", Offer()}}.dump());
    auto stopped = std::async(std::launch::async, [&] { service->Stop(); });
    stopRelease.set_value();
    Require(stopped.wait_for(3s) == std::future_status::ready, "multi-owner shutdown did not finish");
    stopped.get();
    Require(utils::EndpointManager::Instance().Size() == baseline && service->ParticipantCount() == 0 &&
        service->GetStats().active_signaling == 0, "multi-owner shutdown retained sessions");
    Socket released(SOCK_DGRAM);
    Require(released.Bind(config.udp_port), "multi-owner shutdown leaked UDP listener");
    loop.Stop();
}

void RunBackendControls()
{
    const auto baseline = utils::EndpointManager::Instance().Size();
    EventLoop loop(3);
    Require(loop.Start(), "control loop did not start");
    config::AppConfig config;
    config.listen_ip = config.webrtc.public_ip = "127.0.0.1";
    config.udp_port = AvailablePort(SOCK_DGRAM); config.websocket_port = AvailablePort(SOCK_STREAM);
    config.webrtc.enabled = true; config.webrtc.api_keys = {{kApiKey, kApiSecret}};
    config.webrtc.reconnect_timeout_ms = 5000;
    auto service = std::make_shared<server::WebRtcService>(&loop, config, nullptr);
    Require(service->Start(), "control service did not start");
    const auto token = [](const std::string& identity, const std::string& epoch) {
        return RoomToken(identity, "backend", true, true, 0, kApiKey, kApiSecret, false, true, epoch);
    };
    Client member(config.websocket_port), peer(config.websocket_port), stale(config.websocket_port);
    const auto join = [&](Client& client, const std::string& identity, const std::string& epoch) {
        return client.Request({{"type", "join"}, {"room_id", "backend"}, {"token", token(identity, epoch)}});
    };
    const auto joined = join(member, "member", "epoch-one");
    Require(joined.value("type", "") == "joined", "assigned join failed");
    Require(join(peer, "peer", "epoch-one").value("type", "") == "joined", "assigned peer join failed");
    Require(service->ControlRoom("backend", "epoch-wrong", {{"type", "end_room"}}).value("code", "") == "stale_assignment",
        "wrong epoch ended active room");
    Require(service->ControlRoom("backend", "epoch-one", {{"type", "reclaim_room"}}).at("released") == false,
        "reclaimed occupied room");
    const auto roster = service->ControlRoom("backend", "epoch-one", {{"type", "participants"}});
    Require(roster.at("participants").size() == 2, "backend list lost members");
    const auto publication = member.Request({{"type", "publish"}, {"sdp", Offer()}});
    CheckAnswer(publication, config.udp_port);
    const auto updated = service->ControlRoom("backend", "epoch-one", {{"type", "update_participant"},
        {"participant_id", joined.at("participant_id")}, {"name", "backend name"}, {"permissions", {{"can_publish", false}}}});
    Require(updated.at("media_stopped") == true && !utils::EndpointManager::Instance().Find(publication.at("session_id").get<uint64_t>()),
        "backend revoke retained media");
    member.Close();
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!service->GetStats().reconnecting_participants && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(10ms);
    Require(service->ControlRoom("backend", "epoch-one", {{"type", "reclaim_room"}}).at("released") == false,
        "reclaimed room during recovery grace");
    Require(service->ControlRoom("backend", "epoch-one", {{"type", "end_room"}}).at("released") == true, "backend end failed");
    Require(peer.Receive().value("type", "") == "removed", "end omitted terminal notification");
    Require(stale.Request({{"type", "resume"}, {"room_id", "backend"}, {"token", token("member", "epoch-one")},
        {"participant_id", joined.at("participant_id")}, {"resume_token", joined.at("resume_token")}}).value("type", "") == "error",
        "ended room recovered participant");
    Require(join(stale, "stale", "epoch-one").value("type", "") == "error", "retired JWT recreated room");
    Require(service->ControlRoom("backend", "epoch-one", {{"type", "end_room"}}).at("released") == true, "duplicate end failed");
    const auto freshJoined = join(stale, "fresh", "epoch-two");
    Require(freshJoined.value("type", "") == "joined", "new epoch join failed");
    Require(service->ControlRoom("backend", "epoch-one", {{"type", "end_room"}}).value("code", "") == "stale_assignment",
        "old end removed new room");
    // 超时尚未开始的命令不能在 owner 恢复后迟到执行。
    const auto executors = loop.GetTaskSchedulers();
    const auto owner = executors[1 + server::signaling::SignalSessionCoordinator::SelectOwner("backend", executors.size() - 1)];
    std::promise<void> entered, release;
    auto gate = release.get_future().share(); auto ready = entered.get_future();
    owner->Post([&entered, gate] { entered.set_value(); gate.wait_for(4s); });
    Require(ready.wait_for(2s) == std::future_status::ready, "control gate failed");
    const auto timeout = service->ControlRoom("backend", "epoch-two", {{"type", "end_room"}});
    release.set_value();
    Require(timeout.value("code", "") == "unavailable", "blocked owner falsely confirmed command");
    Require(service->ControlRoom("backend", "epoch-two", {{"type", "participants"}}).at("participants").size() == 1,
        "cancelled command executed after deadline");
    Require(service->ControlRoom("backend", "epoch-two", {{"type", "remove_participant"},
        {"participant_id", freshJoined.at("participant_id")}}).value("type", "") == "participant_removed", "last backend removal failed");
    Require(stale.Receive().value("type", "") == "removed", "last member removal omitted notification");
    Client late(config.websocket_port);
    Require(join(late, "late", "epoch-two").value("type", "") == "error", "last removal did not retire assignment");
    Require(service->ControlRoom("unused", "reserved", {{"type", "reclaim_room"}}).at("released") == true,
        "unused reservation did not reclaim");
    service->Stop(); loop.Stop();
    Require(utils::EndpointManager::Instance().Size() == baseline, "backend controls leaked media");
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
    recovery["token"] = RoomToken("observer", "secure", false, false, UnixNow() - 120);
    Require(stranger.Request(recovery).value("code", "") == "token_expired", "expired JWT resumed participant");
    recovery["token"] = RoomToken("observer", "secure", true, true);
    const auto resumed = stranger.Request(recovery);
    Require(resumed.value("type", "") == "resumed" && resumed.value("subject_id", "") == "observer" &&
        resumed.at("participant_id") == joined.at("participant_id"), "same identity could not resume with refreshed admission");
    Require(stranger.Request(publication).value("code", "") == "forbidden", "resume lost publish restriction");
    Require(stranger.Request({{"type", "subscribe"}, {"sdp", Offer()}, {"tracks", Json::array()}}).value("code", "") == "forbidden",
        "refreshed JWT elevated runtime subscription permission");

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
    auto& streams = media::MediaStreamRegistry::Instance();
    const auto streamBaseline = streams.Size();
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
    const auto firstJoined = Join(first, "first", "admission");
    const Json publication = {{"type", "publish"}, {"sdp", Offer()}};
    bad = publication;
    bad["sdp"] = "not SDP";
    Require(first.Request(bad).value("type", "") == "error", "invalid SDP accepted");
    bad["sdp"] = std::string(65537, 'x');
    Require(first.Request(bad).value("type", "") == "error", "oversized SDP accepted");
    Require(utils::EndpointManager::Instance().Size() == baseline, "rejected request allocated endpoint");
    auto inactiveOffer = Offer();
    for (auto offset = inactiveOffer.find("a=sendonly"); offset != std::string::npos;
         offset = inactiveOffer.find("a=sendonly"))
        inactiveOffer.replace(offset, std::string("a=sendonly").size(), "a=inactive");
    Require(first.Request({{"type", "publish"}, {"sdp", inactiveOffer}}).value("type", "") == "error",
        "empty publication unexpectedly accepted");
    Require(service->ParticipantCount() == 1 && first.Request({{"type", "tracks"}}).value("type", "") == "tracks",
        "failed media creation removed joined participant");
    Require(utils::EndpointManager::Instance().Size() == baseline, "failed media creation leaked endpoint");
    // 在媒体已启动、房间发布前制造注册冲突，验证整条创建链的失败回滚。
    auto occupied = std::make_shared<media::MediaStream>(firstJoined.at("participant_id").get<std::string>(),
        media::MediaStream::Source{"test", "admission", {}});
    Require(occupied->ReplaceTracks({{0, media::MediaType::Video, media::CodecType::H264}}) && streams.Register(occupied),
        "could not reserve test stream identity");
    Require(first.Request(publication).value("type", "") == "error" &&
        utils::EndpointManager::Instance().Size() == baseline && streams.Size() == streamBaseline + 1 &&
        streams.FindById(occupied->GetId()) == occupied && service->ParticipantCount() == 1,
        "failed publication leaked resources or removed an existing stream");
    Require(streams.Remove(occupied), "test stream identity was removed by failed publication");
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
    Require(streams.Size() == streamBaseline, "service shutdown leaked core publications");
}
}

int main(int argc, char** argv)
{
    std::signal(SIGPIPE, SIG_IGN);
    try
    {
        if (argc == 2 && std::string(argv[1]) == "--members")
        {
            RunMembers();
            std::cout << "PASS meeting membership, profiles, permissions, notifications, recovery and removal\n";
            return 0;
        }
        if (argc == 2 && std::string(argv[1]) == "--control")
        { RunBackendControls(); std::cout << "PASS backend room controls, epochs, reclamation and deadlines\n"; return 0; }
        Run();
        RunSignalingLifecycle();
        RunCoordinatorChannels();
        RunNegotiationGuards();
        RunRoom();
        RunMediaControls();
        RunRoomOwners();
        RunAuthorization();
        std::cout << "PASS WebRTC service JWT authorization, rooms, media stop/retry, subscriptions, renegotiation, admission, cleanup and restart\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL WebRTC service: " << error.what() << '\n';
        return 1;
    }
}
