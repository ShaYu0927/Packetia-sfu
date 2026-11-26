#include "WebSocketAgentRoomClient.h"
#include "third/nlohmann/json.hpp"
#include <libwebsockets.h>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace service::ai
{
namespace
{
using Json = nlohmann::json;
using EventKind = AgentRoomEvent::Kind;
constexpr auto kRoomProtocol = "packetia";
constexpr lws_usec_t kJoinTimeout = 10 * LWS_US_PER_SEC;
constexpr lws_usec_t kLeaveTimeout = 2 * LWS_US_PER_SEC;
constexpr std::size_t kMaxMessageBytes = 1024 * 1024;

struct Connection
{
    enum class State { Connecting, SendJoin, Joining, Joined, SendLeave, Leaving, Done };
    AgentRoomJoin request;
    AgentRoomClient::Completion completion;
    State state = State::Connecting;
    lws* socket = nullptr;
    bool stop_requested = false;
    std::string message;
    std::string address;
    std::string resource;

    void Finish(EventKind kind, const char* error = "")
    {
        if (state == State::Done) return;
        state = State::Done;
        completion({kind, {}, error});
    }

    void Leave()
    {
        if (state == State::Done || std::exchange(stop_requested, true)) return;
        if (state == State::Connecting || state == State::SendJoin)
        {
            Finish(EventKind::Closed);
            lws_set_timeout(socket, PENDING_TIMEOUT_USER_OK, LWS_TO_KILL_ASYNC);
            return;
        }
        lws_set_timer_usecs(socket, kLeaveTimeout);
        if (state == State::Joined) SendLeave();
    }

    void SendLeave()
    {
        state = State::SendLeave;
        lws_callback_on_writable(socket);
    }

    int Write()
    {
        Json body;
        if (state == State::SendJoin)
        {
            body = {{"type", "join"}, {"room_id", request.room_id}, {"token", std::move(request.token)}};
            state = State::Joining;
        }
        else if (state == State::SendLeave)
        {
            body = {{"type", "leave"}};
            state = State::Leaving;
        }
        else return 0;
        const auto payload = body.dump();
        std::vector<unsigned char> buffer(LWS_PRE + payload.size());
        auto* bytes = buffer.data() + LWS_PRE;
        std::memcpy(bytes, payload.data(), payload.size());
        if (lws_write(socket, bytes, payload.size(), LWS_WRITE_TEXT) < static_cast<int>(payload.size()))
        {
            Finish(EventKind::Failed, "agent_signaling_write_failed");
            return -1;
        }
        return 0;
    }

    int Receive(const void* data, std::size_t size)
    {
        if (lws_frame_is_binary(socket) || size > kMaxMessageBytes - message.size())
        {
            Finish(EventKind::Failed, "agent_invalid_signaling_response");
            return -1;
        }
        message.append(static_cast<const char*>(data), size);
        if (!lws_is_final_fragment(socket) || lws_remaining_packet_payload(socket) != 0) return 0;
        const auto body = Json::parse(message, nullptr, false);
        message.clear();
        if (!body.is_object() || !body.contains("type") || !body["type"].is_string())
        {
            Finish(EventKind::Failed, "agent_invalid_signaling_response");
            return -1;
        }
        const auto& type = body["type"].get_ref<const std::string&>();
        if (type == "closed" || type == "removed")
        {
            Finish(EventKind::Closed);
            return -1;
        }
        if (state != State::Joining) return 0;
        if (type == "error")
        {
            Finish(EventKind::Failed, "agent_join_rejected");
            return -1;
        }
        if (type != "joined") return 0;
        const auto participant = body.find("participant_id");
        if (!body.contains("room_id") || body["room_id"] != request.room_id ||
            !body.contains("subject_id") || body["subject_id"] != request.agent_id ||
            participant == body.end() || !participant->is_string() || participant->get_ref<const std::string&>().empty())
        {
            Finish(EventKind::Failed, "agent_invalid_signaling_response");
            return -1;
        }
        if (stop_requested) SendLeave();
        else
        {
            state = State::Joined;
            lws_set_timer_usecs(socket, LWS_SET_TIMER_USEC_CANCEL);
            completion({EventKind::Joined, participant->get<std::string>(), {}});
        }
        return 0;
    }
};

int OnSocket(lws* socket, lws_callback_reasons reason, void*, void* data, std::size_t size)
{
    if (!socket) return 0;
    auto* connection = static_cast<Connection*>(lws_get_opaque_user_data(socket));
    if (!connection) return 0;
    try
    {
        switch (reason)
        {
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            connection->socket = socket;
            connection->state = Connection::State::SendJoin;
            lws_callback_on_writable(socket);
            return 0;
        case LWS_CALLBACK_CLIENT_WRITEABLE: return connection->Write();
        case LWS_CALLBACK_CLIENT_RECEIVE: return connection->Receive(data, size);
        case LWS_CALLBACK_TIMER:
            connection->Finish(EventKind::Failed, connection->stop_requested ? "agent_leave_timeout" : "agent_join_timeout");
            return -1;
        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
            connection->Finish(EventKind::Failed, "agent_signaling_connect_failed");
            return 0;
        case LWS_CALLBACK_CLIENT_CLOSED:
            connection->Finish(EventKind::Failed, "agent_signaling_disconnected");
            return 0;
        case LWS_CALLBACK_WSI_DESTROY:
            connection->socket = nullptr;
            connection->Finish(EventKind::Failed, "agent_signaling_disconnected");
            return 0;
        default: return 0;
        }
    }
    catch (...)
    {
        connection->Finish(EventKind::Failed, "agent_signaling_failed");
        return -1;
    }
}

const lws_protocols kProtocols[] = {
    {kRoomProtocol, OnSocket, 0, 0, 0, nullptr, 0},
    {}
};

class WebSocketAgentRoomClient final : public AgentRoomClient
{
public:
    ~WebSocketAgentRoomClient() override { Stop(); }

    bool Start() override
    {
        if (thread_.joinable()) return false;
        lws_context_creation_info info{};
        info.port = CONTEXT_PORT_NO_LISTEN;
        info.protocols = kProtocols;
        info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
        context_ = lws_create_context(&info);
        if (!context_) return false;
        stopping_ = false;
        try { thread_ = std::thread([this] { Run(); }); }
        catch (...)
        {
            lws_context_destroy(std::exchange(context_, nullptr));
            throw;
        }
        return true;
    }

    void Stop() override
    {
        if (!thread_.joinable()) return;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            lws_cancel_service(context_);
        }
        thread_.join();
        lws_context_destroy(std::exchange(context_, nullptr));
        connections_.clear();
        pending_.clear();
    }

    bool Join(AgentRoomJoin request, Completion completion) override
    {
        return Post([this, request = std::move(request), completion = std::move(completion)](bool execute) mutable {
            if (!execute)
            {
                completion({EventKind::Failed, {}, "agent_signaling_unavailable"});
                return;
            }
            auto connection = std::make_unique<Connection>();
            connection->request = std::move(request);
            connection->completion = std::move(completion);
            const auto key = connection->request.session_id;
            const auto inserted = connections_.try_emplace(key, std::move(connection));
            if (inserted.second) Connect(*inserted.first->second);
            else connection->Finish(EventKind::Failed, "agent_id_conflict");
        });
    }

    void Leave(const std::string& session_id) override
    {
        Post([this, session_id](bool execute) {
            if (!execute) return;
            const auto found = connections_.find(session_id);
            if (found != connections_.end()) found->second->Leave();
        });
    }

private:
    using Command = std::function<void(bool)>;

    bool Post(Command command)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return false;
        pending_.push_back(std::move(command));
        lws_cancel_service(context_);
        return true;
    }

    void Connect(Connection& connection)
    {
        const char* scheme = nullptr;
        const char* address = nullptr;
        const char* path = nullptr;
        int port = 0;
#if LWS_LIBRARY_VERSION_MAJOR >= 5
        auto destroy = [](lws_parse_uri_t* uri) { lws_parse_uri_destroy(&uri); };
        const std::unique_ptr<lws_parse_uri_t, decltype(destroy)> parsed(
            lws_parse_uri_create(connection.request.url.c_str()), destroy);
        if (parsed && !parsed->unix_skt)
        {
            scheme = parsed->scheme;
            address = parsed->host;
            path = parsed->path;
            port = parsed->port;
        }
#else
        auto url = connection.request.url;
        if (lws_parse_uri(url.data(), &scheme, &address, &port, &path) != 0) scheme = nullptr;
#endif
        const bool tls = scheme && std::strcmp(scheme, "wss") == 0;
        if ((!tls && (!scheme || std::strcmp(scheme, "ws") != 0)) || !address || !*address ||
            port < 1 || port > 65535)
        {
            connection.Finish(EventKind::Failed, "agent_invalid_signaling_url");
            return;
        }
        connection.address = address;
        connection.resource = path && *path == '/' ? path : std::string("/") + (path ? path : "");
        lws_client_connect_info info{};
        info.context = context_;
        info.address = connection.address.c_str();
        info.host = info.address;
        info.port = port;
        info.path = connection.resource.c_str();
        info.protocol = kRoomProtocol;
        info.ssl_connection = LCCSCF_HTTP_NO_FOLLOW_REDIRECT;
        if (tls)
        {
            info.ssl_connection |= LCCSCF_USE_SSL;
            info.alpn = "http/1.1";
        }
        info.opaque_user_data = &connection;
        info.pwsi = &connection.socket;
        if (auto* socket = lws_client_connect_via_info(&info))
        {
            connection.socket = socket;
            lws_set_timer_usecs(socket, kJoinTimeout);
        }
        else
        {
            connection.socket = nullptr;
            connection.Finish(EventKind::Failed, "agent_signaling_connect_failed");
        }
    }

    void Run()
    {
        for (;;)
        {
            bool stopping = false;
            std::deque<Command> commands;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                commands.swap(pending_);
                stopping = stopping_;
            }
            for (auto& command : commands) command(!stopping);
            for (auto iter = connections_.begin(); iter != connections_.end();)
            {
                auto& connection = *iter->second;
                if (!connection.socket) iter = connections_.erase(iter);
                else
                {
                    if (stopping) connection.Leave();
                    ++iter;
                }
            }
            if (stopping && connections_.empty()) return;
            if (lws_service(context_, 0) < 0)
            {
                commands.clear();
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    stopping_ = true;
                    commands.swap(pending_);
                }
                for (auto& command : commands) command(false);
                for (auto& entry : connections_) entry.second->Finish(EventKind::Failed, "agent_signaling_failed");
                return;
            }
        }
    }

    std::mutex mutex_;
    bool stopping_ = true;
    lws_context* context_ = nullptr;
    std::deque<Command> pending_;
    std::unordered_map<std::string, std::unique_ptr<Connection>> connections_;
    std::thread thread_;
};
}

std::shared_ptr<AgentRoomClient> MakeAgentRoomClient()
{
    return std::make_shared<WebSocketAgentRoomClient>();
}
}
