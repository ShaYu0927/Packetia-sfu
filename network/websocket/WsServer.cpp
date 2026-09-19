#include "WsServer.h"
#include "WsHeader.h"
#include "WsSession.h"
#include "WsSessionManager.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <list>
#include <iterator>
#include "EventLoop.h"
#include "TaskScheduler.h"
#include <unordered_map>
#include <unordered_set>
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
        WsOutgoingMessage outgoing;
        std::size_t offset = 0;
    };

public:
    Impl(EventLoop* loop, std::shared_ptr<TaskScheduler> scheduler, WsServerOptions options)
        : options_(std::move(options)), event_loop_(loop), scheduler_(std::move(scheduler)),
          send_budget_(std::make_shared<WsSendBudget>(options_.limits.max_total_queued_bytes,
                                                     options_.limits.max_total_queued_messages))
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
        if (port == 0 || !ValidOptions()) return false;
        // EventLoop restart replaces its schedulers. Finish the old context
        // on its old owner before publishing a new binding.
        auto old = GetScheduler(false);
        if (old && old->IsStopped())
            old->Invoke([self = shared_from_this(), old] { self->StopOnOwner(old); });
        auto scheduler = GetScheduler(true);
        if (!scheduler || !scheduler->IsStarted() || scheduler->IsStopped()) return false;
        bool result = false;
        scheduler->Invoke([self = shared_from_this(), &result, &ip, port, scheduler] {
            std::lock_guard<std::recursive_mutex> lock(self->context_mutex_);
            if (self->scheduler_ != scheduler) return;
            if (!scheduler->IsCurrentThread() || scheduler->IsStopped()) return;
            result = self->StartOnOwner(ip, port);
        });
        return result;
    }

    void Stop()
    {
        auto scheduler = GetScheduler(false);
        if (scheduler)
            scheduler->Invoke([self = shared_from_this(), scheduler] { self->StopOnOwner(scheduler); });
    }

    bool SendText(const std::string& id, const std::string& message)
    {
        if (stopping_) return false;
        auto session = sessions_.GetSession(id);
        const bool accepted = session && session->SendText(message);
        if (!accepted) ++rejected_messages_;
        return accepted;
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

    WsServerStats GetStats() const
    {
        const auto queued = send_budget_->GetStats();
        return {sessions_.GetSessionCount(), queued.bytes, queued.messages,
                accepted_connections_.load(), rejected_connections_.load(),
                received_messages_.load(), sent_messages_.load(), rejected_messages_.load()};
    }

private:
    bool ValidOptions() const
    {
        const auto& limits = options_.limits;
        return limits.max_connections > 0 && limits.max_message_bytes > 0 &&
            limits.max_queued_bytes > 0 && limits.max_queued_messages > 0 &&
            limits.max_total_queued_bytes > 0 && limits.max_total_queued_messages > 0 &&
            options_.service_interval_ms > 0 && options_.max_writable_batch > 0 &&
            options_.max_pending_handshakes > 0 && options_.handshake_timeout_seconds > 0 &&
            limits.max_connections <= static_cast<std::size_t>(std::numeric_limits<int>::max()) / 2 &&
            options_.max_pending_handshakes <= static_cast<std::size_t>(std::numeric_limits<int>::max()) / 2 - 16;
    }

    std::shared_ptr<TaskScheduler> GetScheduler(bool bind)
    {
        // The hot sending path only snapshots the owner; it does not wait on
        // application callbacks or the context lock.
        std::unique_lock<std::recursive_mutex> context_lock(context_mutex_, std::defer_lock);
        if (bind) context_lock.lock();
        std::lock_guard<std::mutex> lock(owner_mutex_);
        if (bind && event_loop_ && (!scheduler_ || scheduler_->IsStopped()) &&
            !context_ && !servicing_ && !destroying_)
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
        info.fd_limit_per_thread = static_cast<unsigned int>(options_.limits.max_connections +
            options_.max_pending_handshakes + 16);
        info.max_http_header_pool2 = static_cast<unsigned int>(options_.max_pending_handshakes);
        info.timeout_secs = options_.handshake_timeout_seconds;
        info.timeout_secs_ah_idle = options_.handshake_timeout_seconds;
        context_ = lws_create_context(&info);
        if (!context_) return false;
        const auto generation = ++generation_;
        service_posted_ = false;
        stopping_ = false;
        auto scheduler = scheduler_;
        std::weak_ptr<Impl> weak = shared_from_this();
        timer_ = scheduler->AddTimer([weak, scheduler, generation] {
            auto self = weak.lock();
            if (!self) return false;
            self->ServiceOnOwner(scheduler, generation);
            return !self->stopping_ && self->generation_ == generation;
        }, options_.service_interval_ms);
        return true;
    }

    void NotifyWritable(const std::string& id)
    {
        if (stopping_) return;
        {
            std::lock_guard<std::mutex> lock(ready_mutex_);
            if (ready_index_.find(id) == ready_index_.end())
            {
                ready_.push_back(id);
                ready_index_.emplace(id, std::prev(ready_.end()));
            }
        }
        RequestService();
    }

    void ArmWritable()
    {
        // At most one ready entry per connection, O(1) lookup and removal.
        // Idle connections are never scanned or locked on a service tick.
        for (std::size_t i = 0; i < options_.max_writable_batch; ++i)
        {
            std::string id;
            {
                std::lock_guard<std::mutex> lock(ready_mutex_);
                if (ready_.empty()) break;
                id = std::move(ready_.front());
                ready_.pop_front();
                ready_index_.erase(id);
            }
            const auto found = sockets_by_id_.find(id);
            if (found != sockets_by_id_.end()) lws_callback_on_writable(found->second);
        }
    }

    void RequestService()
    {
        if (stopping_ || service_posted_.exchange(true)) return;
        auto scheduler = GetScheduler(false);
        const auto generation = generation_.load();
        std::weak_ptr<Impl> weak = shared_from_this();
        if (!scheduler || !scheduler->Post([weak, scheduler, generation] {
                if (auto self = weak.lock())
                {
                    std::lock_guard<std::recursive_mutex> lock(self->context_mutex_);
                    if (self->generation_ != generation || self->scheduler_ != scheduler) return;
                    self->service_posted_ = false;
                    self->ServiceOnOwner(scheduler, generation);
                }
            }))
            service_posted_ = false; // periodic timer retries pending work
    }

    void ServiceOnOwner(const std::shared_ptr<TaskScheduler>& scheduler, uint64_t generation)
    {
        std::lock_guard<std::recursive_mutex> lock(context_mutex_);
        if (scheduler_ != scheduler || generation_ != generation) return;
        if (!context_ || servicing_ || destroying_) return;
        if (stopping_ || scheduler->IsStopped())
        {
            StopOnOwner(scheduler);
            return;
        }
        servicing_ = true;
        ArmWritable();
        // Negative timeout disables the poll backend's internal wait. Zero
        // may block for the next lws timer and must not be used on this loop.
        const int result = lws_service(context_, -1);
        servicing_ = false;
        if (result < 0 || stopping_ || scheduler->IsStopped()) StopOnOwner(scheduler);
        bool pending = false;
        {
            std::lock_guard<std::mutex> ready_lock(ready_mutex_);
            pending = !ready_.empty();
        }
        if (pending) RequestService();
    }

    void StopOnOwner(const std::shared_ptr<TaskScheduler>& scheduler)
    {
        // Invoke serializes work while Run is alive. This lock also serializes
        // cleanup after Run exits, when Invoke executes on the calling thread.
        // It is recursive because a callback is allowed to stop its server.
        std::lock_guard<std::recursive_mutex> lock(context_mutex_);
        if (scheduler_ != scheduler) return;
        stopping_ = true;
        if (timer_)
        {
            scheduler->RemoveTimer(timer_);
            timer_ = 0;
        }
        // A callback can request Stop or destroy WsServer. The shared owner in
        // ServiceOnOwner's caller keeps Impl alive until lws unwinds.
        if (servicing_ || destroying_ || !context_) return;
        destroying_ = true;
        ++generation_;
        service_posted_ = false;
        auto* context = context_;
        context_ = nullptr;
        lws_context_destroy(context);
        while (!connections_.empty())
        {
            try { DropConnection(connections_.begin()->first); }
            catch (...) { lwsl_err("Packetia: close callback threw during shutdown\n"); }
        }
        {
            std::lock_guard<std::mutex> ready_lock(ready_mutex_);
            ready_.clear();
            ready_index_.clear();
        }
        sockets_by_id_.clear();
        pending_handshakes_.clear();
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
        case LWS_CALLBACK_FILTER_NETWORK_CONNECTION:
            if (stopping_ || pending_handshakes_.size() >= options_.max_pending_handshakes ||
                sessions_.GetSessionCount() >= options_.limits.max_connections)
            {
                ++rejected_connections_;
                return -1;
            }
            return 0;
        case LWS_CALLBACK_SERVER_NEW_CLIENT_INSTANTIATED:
            pending_handshakes_.insert(wsi);
            return 0;
        case LWS_CALLBACK_FILTER_PROTOCOL_CONNECTION:
            if (stopping_ || sessions_.GetSessionCount() >= options_.limits.max_connections)
            {
                ++rejected_connections_;
                return -1;
            }
            return 0;
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
                ++rejected_messages_;
                lws_close_reason(wsi, LWS_CLOSE_STATUS_MESSAGE_TOO_LARGE, nullptr, 0);
                return -1;
            }
            return 0;
        }
        case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
            if (!stopping_) ArmWritable();
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
        pending_handshakes_.erase(wsi);
        if (stopping_ || sessions_.GetSessionCount() >= options_.limits.max_connections)
        {
            ++rejected_connections_;
            return -1;
        }
        const auto id = "WS_" + std::to_string(next_id_++);
        std::weak_ptr<Impl> weak = shared_from_this();
        auto session = std::make_shared<WsSession>(id, [weak, id] {
            if (auto self = weak.lock()) self->NotifyWritable(id);
        }, options_.limits, send_budget_);
        session->SetOnMessage([this](const std::string& sid, const std::string& message) {
            ++received_messages_;
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
        sockets_by_id_.emplace(id, wsi);
        if (!sessions_.AddSession(session, options_.limits.max_connections))
        {
            connections_.erase(wsi);
            sockets_by_id_.erase(id);
            session->OnClosed();
            ++rejected_connections_;
            return -1;
        }
        ++accepted_connections_;
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
        if (connection.outgoing.Empty() && !connection.session->PopOutgoing(connection.outgoing))
            return 0;
        const auto remaining = connection.outgoing.Size() - connection.offset;
        const auto count = std::min(remaining, kChunkBytes);
        // All writes run on the owner. lws copies any socket short write into
        // its own buffer, so this scratch storage can be reused across clients.
        if (count) std::memcpy(write_buffer_.data() + LWS_PRE,
                               connection.outgoing.Data().data() + connection.offset, count);
        int flags = connection.offset == 0 ? LWS_WRITE_TEXT : LWS_WRITE_CONTINUATION;
        if (count < remaining) flags |= LWS_WRITE_NO_FIN;
        const int written = lws_write(wsi, write_buffer_.data() + LWS_PRE, count,
                                      static_cast<lws_write_protocol>(flags));
        // lws buffers a short socket write internally and returns the accepted
        // length. A smaller return here is an error, not a retryable fragment.
        if (written < 0 || static_cast<std::size_t>(written) < count) return -1;
        connection.offset += count;
        if (connection.offset == connection.outgoing.Size())
        {
            connection.outgoing.Reset();
            connection.offset = 0;
            ++sent_messages_;
        }
        if (!connection.outgoing.Empty() || connection.session->NeedsWritable())
            lws_callback_on_writable(wsi);
        return 0;
    }

    void DropConnection(lws* wsi)
    {
        pending_handshakes_.erase(wsi);
        auto it = connections_.find(wsi);
        if (it == connections_.end()) return;
        auto session = it->second.session;
        connections_.erase(it);
        sockets_by_id_.erase(session->GetSessionId());
        session->OnClosed();
        sessions_.RemoveSession(session->GetSessionId());
        {
            std::lock_guard<std::mutex> lock(ready_mutex_);
            const auto pending = ready_index_.find(session->GetSessionId());
            if (pending != ready_index_.end())
            {
                ready_.erase(pending->second);
                ready_index_.erase(pending);
            }
        }
        OnCloseCallback callback;
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            callback = on_close_;
        }
        if (callback) callback(session->GetSessionId());
    }

    static constexpr std::size_t kChunkBytes = 16 * 1024;
    const WsServerOptions options_;
    lws_protocols protocols_[2]{};
    lws_retry_bo_t idle_policy_{};
    EventLoop* event_loop_ = nullptr; // must outlive the public WsServer
    std::mutex owner_mutex_, callback_mutex_;
    std::recursive_mutex context_mutex_;
    std::shared_ptr<TaskScheduler> scheduler_;
    lws_context* context_ = nullptr; // scheduler thread only
    TimeId timer_ = 0;
    bool servicing_ = false;
    bool destroying_ = false;
    std::atomic_bool stopping_{true}, service_posted_{false};
    std::atomic<uint64_t> generation_{0};
    std::string bind_ip_;
    uint64_t next_id_ = 0; // service thread only; never reused after a restart
    WsSessionManager sessions_;
    std::shared_ptr<WsSendBudget> send_budget_;
    std::unordered_map<lws*, Connection> connections_;
    std::unordered_set<lws*> pending_handshakes_;
    std::unordered_map<std::string, lws*> sockets_by_id_;
    std::mutex ready_mutex_;
    std::list<std::string> ready_;
    std::unordered_map<std::string, std::list<std::string>::iterator> ready_index_;
    std::array<unsigned char, LWS_PRE + kChunkBytes + 1> write_buffer_{};
    std::atomic<uint64_t> accepted_connections_{0}, rejected_connections_{0};
    std::atomic<uint64_t> received_messages_{0}, sent_messages_{0}, rejected_messages_{0};
    OnOpenCallback on_open_;
    OnMessageCallback on_message_;
    OnCloseCallback on_close_;
};

WsServer::WsServer(EventLoop* event_loop, WsServerOptions options)
    : impl_(std::make_shared<Impl>(event_loop, nullptr, std::move(options))) {}
WsServer::WsServer(std::shared_ptr<TaskScheduler> scheduler, WsServerOptions options)
    : impl_(std::make_shared<Impl>(nullptr, std::move(scheduler), std::move(options))) {}
WsServer::~WsServer() { impl_->Stop(); }
bool WsServer::Start(const std::string& ip, uint16_t port)
{ return impl_->Start(ip, port); }
void WsServer::Stop() { impl_->Stop(); }
bool WsServer::SendText(const std::string& id, const std::string& message)
{ return impl_->SendText(id, message); }
bool WsServer::CloseConnection(const std::string& id)
{ return impl_->CloseConnection(id); }
WsServerStats WsServer::GetStats() const { return impl_->GetStats(); }
void WsServer::SetOnOpen(OnOpenCallback cb) { impl_->SetOnOpen(std::move(cb)); }
void WsServer::SetOnMessage(OnMessageCallback cb) { impl_->SetOnMessage(std::move(cb)); }
void WsServer::SetOnClose(OnCloseCallback cb) { impl_->SetOnClose(std::move(cb)); }
}
}
