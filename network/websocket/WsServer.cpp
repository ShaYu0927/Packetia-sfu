#include "WsServer.h"
#include "WsHeader.h"
#include "WsSession.h"
#include "WsSessionManager.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include "EventLoop.h"
#include "TaskScheduler.h"
#include <unordered_map>
#include <utility>
#include <vector>

namespace network
{
namespace websocket
{
class WsServer::Impl : public std::enable_shared_from_this<WsServer::Impl>
{
    struct Connection
    {
        WsSession::Ptr session;
        std::string outgoing;
        std::size_t offset = 0;
    };

public:
    Impl(EventLoop* loop, std::shared_ptr<TaskScheduler> scheduler)
        : event_loop_(loop), scheduler_(std::move(scheduler))
    {
        protocols_[0].name = "packetia";
        protocols_[0].callback = &Callback;
        protocols_[0].rx_buffer_size = kChunkBytes;
        protocols_[0].tx_packet_size = kChunkBytes;
        idle_policy_.secs_since_valid_ping = 30;
        idle_policy_.secs_since_valid_hangup = 60;
    }

    bool Start(const std::string& ip, uint16_t port)
    {
        if (port == 0) return false;
        auto scheduler = GetScheduler(true);
        if (!scheduler || !scheduler->IsStarted() || scheduler->IsStopped()) return false;
        bool result = false;
        scheduler->Invoke([self = shared_from_this(), &result, &ip, port, scheduler] {
            if (!scheduler->IsCurrentThread() || scheduler->IsStopped()) return;
            result = self->StartOnOwner(ip, port);
        });
        return result;
    }

    void Stop()
    {
        auto scheduler = GetScheduler(false);
        if (scheduler)
            scheduler->Invoke([self = shared_from_this()] { self->StopOnOwner(); });
    }

    bool SendText(const std::string& id, const std::string& message)
    {
        if (stopping_) return false;
        auto session = sessions_.GetSession(id);
        return session && session->SendText(message);
    }

    bool CloseConnection(const std::string& id)
    {
        auto session = sessions_.GetSession(id);
        if (!session || session->IsClosing()) return false;
        session->Close();
        return true;
    }

    void SetOnOpen(OnOpenCallback cb)
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        on_open_ = std::move(cb);
    }
    void SetOnMessage(OnMessageCallback cb)
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        on_message_ = std::move(cb);
    }
    void SetOnClose(OnCloseCallback cb)
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        on_close_ = std::move(cb);
    }

private:
    std::shared_ptr<TaskScheduler> GetScheduler(bool bind)
    {
        std::lock_guard<std::mutex> lock(owner_mutex_);
        if (bind && !scheduler_ && event_loop_)
            scheduler_ = event_loop_->GetTaskScheduler();
        return scheduler_;
    }

    bool StartOnOwner(const std::string& ip, uint16_t port)
    {
        if (servicing_ || destroying_) return false;
        if (context_) return !stopping_;
        bind_ip_ = ip;
        lws_context_creation_info info{};
        info.port = port;
        info.iface = (ip.empty() || ip == "0.0.0.0" || ip == "::") ? nullptr : bind_ip_.c_str();
        info.protocols = protocols_;
        info.user = this;
        info.gid = -1;
        info.uid = -1;
        info.options = LWS_SERVER_OPTION_VALIDATE_UTF8;
        info.retry_and_idle_policy = &idle_policy_;
        context_ = lws_create_context(&info);
        if (!context_) return false;
        stopping_ = false;
        std::weak_ptr<Impl> weak = shared_from_this();
        timer_ = scheduler_->AddTimer([weak] {
            auto self = weak.lock();
            if (!self) return false;
            self->ServiceOnOwner();
            return !self->stopping_;
        }, kServiceIntervalMs);
        return true;
    }

    void RequestService()
    {
        if (stopping_ || service_posted_.exchange(true)) return;
        auto scheduler = GetScheduler(false);
        std::weak_ptr<Impl> weak = shared_from_this();
        if (!scheduler || !scheduler->Post([weak] {
                if (auto self = weak.lock())
                {
                    self->service_posted_ = false;
                    self->ServiceOnOwner();
                }
            }))
            service_posted_ = false; // periodic timer retries pending work
    }

    void ServiceOnOwner()
    {
        if (!context_ || servicing_ || destroying_) return;
        if (stopping_ || scheduler_->IsStopped())
        {
            StopOnOwner();
            return;
        }
        servicing_ = true;
        for (auto& entry : connections_)
            if (entry.second.session->NeedsWritable() || !entry.second.outgoing.empty())
                lws_callback_on_writable(entry.first);
        // Negative timeout disables the poll backend's internal wait. Zero
        // may block for the next lws timer and must not be used on this loop.
        const int result = lws_service(context_, -1);
        servicing_ = false;
        if (result < 0 || stopping_ || scheduler_->IsStopped()) StopOnOwner();
    }

    void StopOnOwner()
    {
        stopping_ = true;
        if (timer_)
        {
            scheduler_->RemoveTimer(timer_);
            timer_ = 0;
        }
        // A callback can request Stop or destroy WsServer. The shared owner in
        // ServiceOnOwner's caller keeps Impl alive until lws unwinds.
        if (servicing_ || destroying_ || !context_) return;
        destroying_ = true;
        auto* context = context_;
        context_ = nullptr;
        lws_context_destroy(context);
        while (!connections_.empty())
        {
            try { DropConnection(connections_.begin()->first); }
            catch (...) { lwsl_err("Packetia: close callback threw during shutdown\n"); }
        }
        destroying_ = false;
    }

    static int Callback(lws* wsi, lws_callback_reasons reason, void* user, void* in, size_t len)
    {
        if (!wsi) return 0;
        auto* self = static_cast<Impl*>(lws_context_user(lws_get_context(wsi)));
        if (!self) return 0;
        // Exceptions from application callbacks must never cross the C ABI.
        try { return self->Handle(wsi, reason, user, in, len); }
        catch (...)
        {
            lwsl_err("Packetia: WebSocket callback threw; closing connection\n");
            return -1;
        }
    }

    int Handle(lws* wsi, lws_callback_reasons reason, void* user, void* in, size_t len)
    {
        switch (reason)
        {
        case LWS_CALLBACK_ESTABLISHED:
            return OpenConnection(wsi);
        case LWS_CALLBACK_RECEIVE:
        {
            auto it = connections_.find(wsi);
            if (it == connections_.end() || stopping_) return -1;
            if (it->second.session->IsClosing()) return -1;
            if (lws_frame_is_binary(wsi))
            {
                lws_close_reason(wsi, LWS_CLOSE_STATUS_UNACCEPTABLE_OPCODE, nullptr, 0);
                return -1;
            }
            const bool final = lws_is_final_fragment(wsi) && lws_remaining_packet_payload(wsi) == 0;
            if (!it->second.session->ReceiveFragment(in, len, final))
            {
                lws_close_reason(wsi, LWS_CLOSE_STATUS_MESSAGE_TOO_LARGE, nullptr, 0);
                return -1;
            }
            return 0;
        }
        case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
            if (!stopping_)
                for (auto& entry : connections_)
                    if (entry.second.session->NeedsWritable() || !entry.second.outgoing.empty())
                        lws_callback_on_writable(entry.first);
            return 0;
        case LWS_CALLBACK_SERVER_WRITEABLE:
            return Write(wsi);
        case LWS_CALLBACK_CLOSED:
        case LWS_CALLBACK_WSI_DESTROY:
            DropConnection(wsi);
            return 0;
        case LWS_CALLBACK_HTTP:
            lws_return_http_status(wsi, HTTP_STATUS_NOT_FOUND, nullptr);
            return -1;
        default:
            return lws_callback_http_dummy(wsi, reason, user, in, len);
        }
    }

    int OpenConnection(lws* wsi)
    {
        if (stopping_) return -1;
        const auto id = "WS_" + std::to_string(next_id_++);
        std::weak_ptr<Impl> weak = shared_from_this();
        auto session = std::make_shared<WsSession>(id, [weak] {
            if (auto self = weak.lock()) self->RequestService();
        });
        session->SetOnMessage([this](const std::string& sid, const std::string& message) {
            OnMessageCallback callback;
            {
                std::lock_guard<std::mutex> lock(callback_mutex_);
                callback = on_message_;
            }
            if (callback)
            {
                auto reply = callback(sid, message);
                // Empty callback result means no automatic response.
                if (!reply.empty() && !SendText(sid, reply))
                    CloseConnection(sid);
            }
        });
        session->OnOpen();
        connections_.emplace(wsi, Connection{session, {}, 0});
        if (!sessions_.AddSession(session))
        {
            connections_.erase(wsi);
            session->OnClosed();
            return -1;
        }
        WsConnectionInfo info;
        info.connId = id;
        const int path_size = lws_hdr_total_length(wsi, WSI_TOKEN_GET_URI);
        if (path_size > 0)
        {
            std::vector<char> path(static_cast<std::size_t>(path_size) + 1);
            if (lws_hdr_copy(wsi, path.data(), static_cast<int>(path.size()), WSI_TOKEN_GET_URI) >= 0)
                info.path = path.data();
        }
        if (info.path.empty()) info.path = "/";
        char peer[128]{};
        lws_get_peer_simple(wsi, peer, sizeof(peer));
        info.peerAddr = peer;
        OnOpenCallback callback;
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            callback = on_open_;
        }
        if (callback) callback(info);
        return 0;
    }

    int Write(lws* wsi)
    {
        auto it = connections_.find(wsi);
        if (it == connections_.end()) return -1;
        auto& connection = it->second;
        if (stopping_ || connection.session->IsClosing())
        {
            lws_close_reason(wsi, LWS_CLOSE_STATUS_NORMAL, nullptr, 0);
            return -1;
        }
        if (connection.outgoing.empty() && !connection.session->PopOutgoing(connection.outgoing))
            return 0;
        const auto remaining = connection.outgoing.size() - connection.offset;
        const auto count = std::min(remaining, kChunkBytes);
        std::vector<unsigned char> buffer(LWS_PRE + count + 1);
        if (count) std::memcpy(buffer.data() + LWS_PRE, connection.outgoing.data() + connection.offset, count);
        int flags = connection.offset == 0 ? LWS_WRITE_TEXT : LWS_WRITE_CONTINUATION;
        if (count < remaining) flags |= LWS_WRITE_NO_FIN;
        const int written = lws_write(wsi, buffer.data() + LWS_PRE, count,
                                      static_cast<lws_write_protocol>(flags));
        // lws buffers a short socket write internally and returns the accepted
        // length. A smaller return here is an error, not a retryable fragment.
        if (written < 0 || static_cast<std::size_t>(written) < count) return -1;
        connection.offset += count;
        if (connection.offset == connection.outgoing.size())
        {
            connection.outgoing.clear();
            connection.offset = 0;
        }
        if (!connection.outgoing.empty() || connection.session->NeedsWritable())
            lws_callback_on_writable(wsi);
        return 0;
    }

    void DropConnection(lws* wsi)
    {
        auto it = connections_.find(wsi);
        if (it == connections_.end()) return;
        auto session = it->second.session;
        connections_.erase(it);
        session->OnClosed();
        sessions_.RemoveSession(session->GetSessionId());
        OnCloseCallback callback;
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            callback = on_close_;
        }
        if (callback) callback(session->GetSessionId());
    }

    static constexpr std::size_t kChunkBytes = 16 * 1024;
    static constexpr uint32_t kServiceIntervalMs = 5;
    lws_protocols protocols_[2]{};
    lws_retry_bo_t idle_policy_{};
    EventLoop* event_loop_ = nullptr; // must outlive the public WsServer
    std::mutex owner_mutex_, callback_mutex_;
    std::shared_ptr<TaskScheduler> scheduler_;
    lws_context* context_ = nullptr; // scheduler thread only
    TimeId timer_ = 0;
    bool servicing_ = false;
    bool destroying_ = false;
    std::atomic_bool stopping_{true}, service_posted_{false};
    std::string bind_ip_;
    uint64_t next_id_ = 0; // service thread only; never reused after a restart
    WsSessionManager sessions_;
    std::unordered_map<lws*, Connection> connections_;
    OnOpenCallback on_open_;
    OnMessageCallback on_message_;
    OnCloseCallback on_close_;
};

WsServer::WsServer(EventLoop* event_loop)
    : impl_(std::make_shared<Impl>(event_loop, nullptr)) {}
WsServer::WsServer(std::shared_ptr<TaskScheduler> scheduler)
    : impl_(std::make_shared<Impl>(nullptr, std::move(scheduler))) {}
WsServer::~WsServer() { impl_->Stop(); }
bool WsServer::Start(const std::string& ip, uint16_t port)
{ return impl_->Start(ip, port); }
void WsServer::Stop() { impl_->Stop(); }
bool WsServer::SendText(const std::string& id, const std::string& message)
{ return impl_->SendText(id, message); }
bool WsServer::CloseConnection(const std::string& id)
{ return impl_->CloseConnection(id); }
void WsServer::SetOnOpen(OnOpenCallback cb) { impl_->SetOnOpen(std::move(cb)); }
void WsServer::SetOnMessage(OnMessageCallback cb) { impl_->SetOnMessage(std::move(cb)); }
void WsServer::SetOnClose(OnCloseCallback cb) { impl_->SetOnClose(std::move(cb)); }
}
}
