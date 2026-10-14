#include "HttpServer.h"
#include "utils/EventLoop.h"
#include "utils/TaskScheduler.h"
#include "utils/HttpUtil.h"
#include "log/logger.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef PACKETIA_HTTP_WITH_LIBWEBSOCKETS
#include <libwebsockets.h>
#endif

namespace protocol::http
{
#ifdef PACKETIA_HTTP_WITH_LIBWEBSOCKETS
namespace
{
constexpr std::size_t kWriteChunk = 16 * 1024;

}

class HttpServer::Impl : public std::enable_shared_from_this<Impl>
{
    struct Slot
    {
        bool answered = false;
        bool close = false;
        std::size_t bytes = 0;
        std::optional<HttpResponse> response;
    };
    struct Connection
    {
        std::uint64_t request_id = 0;
        HttpRequest request;
        std::size_t expected_body = 0;
        bool dispatched = false;
        bool headers_sent = false;
        bool close_after_response = false;
        bool head = false;
        HttpResponse response;
        bool has_response = false;
        std::size_t offset = 0;
    };

public:
    Impl(EventLoop* loop, std::shared_ptr<TaskScheduler> scheduler)
        : loop_(loop), scheduler_(std::move(scheduler))
    {
        protocols_[0].name = "http";
        protocols_[0].callback = &Callback;
        protocols_[0].rx_buffer_size = kWriteChunk;
        protocols_[0].tx_packet_size = kWriteChunk;
    }

    bool Start(const HttpServerOptions& options, RequestHandler handler)
    {
        if (!options.enabled) return true;
        if (!options.IsValid() || !handler) return false;
        auto old = Owner(false);
        if (old && old->IsStopped())
            old->Invoke([self = shared_from_this(), old] { self->StopOnOwner(old); });
        auto owner = Owner(true);
        if (!owner || !owner->IsStarted() || owner->IsStopped())
        {
            LOG_ERROR("[HttpServer] Start requires a running EventLoop or TaskScheduler");
            return false;
        }
        bool result = false;
        owner->Invoke([self = shared_from_this(), owner, &result, options, handler = std::move(handler)]() mutable {
            std::lock_guard<std::recursive_mutex> lock(self->context_mutex_);
            if (self->scheduler_ != owner || owner->IsStopped() || !owner->IsCurrentThread()) return;
            if (self->context_) { result = !self->stopping_; return; }
            if (self->servicing_ || self->destroying_) return;
            {
                std::lock_guard<std::mutex> mail_lock(self->mail_mutex_);
                self->options_ = options;
            }
            self->handler_ = std::move(handler);
            lws_context_creation_info info{};
            info.port = options.listen_port;
            info.iface = self->options_.listen_ip.c_str();
            info.protocols = self->protocols_;
            info.user = self.get();
            info.gid = info.uid = -1;
            info.alpn = "http/1.1";
            info.max_http_header_data2 = static_cast<unsigned int>(options.max_header_bytes);
            info.max_http_header_pool2 = static_cast<unsigned int>(options.max_connections);
            info.fd_limit_per_thread = static_cast<unsigned int>(options.max_connections + 16);
            info.timeout_secs = options.request_timeout_seconds;
            info.timeout_secs_ah_idle = options.request_timeout_seconds;
            info.keepalive_timeout = static_cast<int>(options.idle_timeout_seconds);
            self->context_ = lws_create_context(&info);
            if (!self->context_) { self->handler_ = {}; return; }
            self->stopping_ = false;
            const auto generation = ++self->generation_;
            {
                std::lock_guard<std::mutex> mail_lock(self->mail_mutex_);
                self->accepting_replies_ = true;
            }
            std::weak_ptr<Impl> weak = self;
            self->timer_ = owner->AddTimer([weak, owner, generation] {
                if (auto server = weak.lock())
                {
                    server->Service(owner, generation);
                    return !server->stopping_ && server->generation_ == generation;
                }
                return false;
            }, options.service_interval_ms);
            if (!self->timer_) { self->StopOnOwner(owner); return; }
            result = true;
        });
        return result;
    }

    void Stop()
    {
        auto owner = Owner(false);
        if (owner) owner->Invoke([self = shared_from_this(), owner] { self->StopOnOwner(owner); });
    }

private:
    std::shared_ptr<TaskScheduler> Owner(bool bind)
    {
        std::unique_lock<std::recursive_mutex> context_lock(context_mutex_, std::defer_lock);
        if (bind) context_lock.lock();
        std::lock_guard<std::mutex> owner_lock(owner_mutex_);
        if (bind && loop_ && (!scheduler_ || scheduler_->IsStopped()) && !context_ && !servicing_ && !destroying_)
            scheduler_ = loop_->GetTaskScheduler();
        return scheduler_;
    }

    void RequestService()
    {
        if (stopping_ || service_posted_.exchange(true)) return;
        auto owner = Owner(false);
        const auto generation = generation_.load();
        std::weak_ptr<Impl> weak = shared_from_this();
        if (!owner || !owner->Post([weak, owner, generation] {
            if (auto self = weak.lock())
            {
                std::lock_guard<std::recursive_mutex> lock(self->context_mutex_);
                if (self->scheduler_ != owner || self->generation_ != generation) return;
                self->service_posted_ = false;
                self->Service(owner, generation);
            }
        })) service_posted_ = false; // The periodic service tick retries.
    }

    void Service(const std::shared_ptr<TaskScheduler>& owner, std::uint64_t generation)
    {
        std::lock_guard<std::recursive_mutex> lock(context_mutex_);
        if (scheduler_ != owner || generation_ != generation || !context_ || servicing_ || destroying_) return;
        if (stopping_ || owner->IsStopped()) { StopOnOwner(owner); return; }
        servicing_ = true;
        ArmReplies();
        const int result = lws_service(context_, -1);
        servicing_ = false;
        if (result < 0 || stopping_ || owner->IsStopped()) StopOnOwner(owner);
    }

    void StopOnOwner(const std::shared_ptr<TaskScheduler>& owner)
    {
        std::lock_guard<std::recursive_mutex> lock(context_mutex_);
        if (scheduler_ != owner) return;
        stopping_ = true;
        {
            std::lock_guard<std::mutex> mail_lock(mail_mutex_);
            accepting_replies_ = false;
            slots_.clear();
            ready_.clear();
            response_bytes_ = 0;
        }
        if (timer_) { owner->RemoveTimer(timer_); timer_ = 0; }
        if (servicing_ || destroying_ || !context_) return;
        destroying_ = true;
        ++generation_;
        service_posted_ = false;
        auto* context = context_;
        context_ = nullptr;
        lws_context_destroy(context);
        connections_.clear();
        sockets_.clear();
        network_connections_.clear();
        handler_ = {};
        destroying_ = false;
    }

    void Retire(std::uint64_t id)
    {
        if (!id) return;
        sockets_.erase(id);
        std::lock_guard<std::mutex> lock(mail_mutex_);
        const auto it = slots_.find(id);
        if (it != slots_.end()) { response_bytes_ -= it->second.bytes; slots_.erase(it); }
        ready_.erase(id);
    }

    void Drop(lws* wsi)
    {
        auto it = connections_.find(wsi);
        if (it != connections_.end()) { Retire(it->second.request_id); connections_.erase(it); }
        network_connections_.erase(wsi);
    }

    void Reply(std::uint64_t id, HttpResponse response)
    {
        std::lock_guard<std::mutex> lock(mail_mutex_);
        auto it = slots_.find(id);
        if (!accepting_replies_ || it == slots_.end() || it->second.answered) return;
        bool invalid = response.status < 200 || response.status > 599 ||
            response.body.size() > options_.max_response_bytes;
        std::size_t header_bytes = 0;
        Headers normalized;
        for (auto& header : response.headers)
        {
            const auto name = utils::ToLowerAscii(header.first);
            if (!utils::IsValidHttpHeaderName(name) || !utils::IsValidHttpHeaderValue(header.second))
            { invalid = true; break; }
            header_bytes += name.size() + header.second.size() + 4;
            if (header_bytes > options_.max_header_bytes || !normalized.emplace(name, std::move(header.second)).second)
            { invalid = true; break; }
        }
        if (!invalid) response.headers = std::move(normalized);
        auto& slot = it->second;
        slot.answered = true;
        const auto bytes = response.body.size() + header_bytes;
        if (invalid || bytes > options_.max_total_response_bytes - response_bytes_) slot.close = true;
        else
        {
            slot.bytes = bytes;
            response_bytes_ += bytes;
            slot.response = std::move(response);
        }
        ready_.insert(id);
        RequestService();
    }

    void ArmReplies()
    {
        std::lock_guard<std::mutex> lock(mail_mutex_);
        for (auto id : ready_)
        {
            const auto socket = sockets_.find(id);
            auto slot = slots_.find(id);
            if (socket == sockets_.end() || slot == slots_.end()) continue;
            auto connection = connections_.find(socket->second);
            if (connection == connections_.end()) continue;
            if (slot->second.close)
                lws_set_timeout(socket->second, PENDING_TIMEOUT_USER_OK, LWS_TO_KILL_ASYNC);
            else if (slot->second.response)
            {
                connection->second.response = std::move(*slot->second.response);
                slot->second.response.reset();
                connection->second.has_response = true;
                lws_callback_on_writable(socket->second);
            }
        }
        ready_.clear();
    }

    void Dispatch(lws* wsi, Connection& connection)
    {
        if (connection.dispatched || stopping_) return;
        connection.dispatched = true;
        const auto id = connection.request_id;
        std::weak_ptr<Impl> weak = shared_from_this();
        auto respond = [weak, id](HttpResponse response) {
            if (auto self = weak.lock()) self->Reply(id, std::move(response));
        };
        try
        {
            // A copy remains valid if a handler stops/destroys its server.
            const auto handler = handler_;
            handler(std::move(connection.request), respond);
        }
        catch (...) { respond({500, "Internal server error", {{"content-type", "text/plain"}}}); }
    }

    int Error(lws* wsi, Connection& connection, std::uint16_t status)
    {
        connection.dispatched = true;
        connection.close_after_response = true;
        Reply(connection.request_id, {status, {}, {}});
        return 0;
    }

    bool ReadHeaders(lws* wsi, HttpRequest& request)
    {
        for (int i = 0; i < WSI_TOKEN_COUNT; ++i)
        {
            const auto token = static_cast<lws_token_indexes>(i);
            const auto* text = lws_token_to_string(token);
            if (!text) continue;
            std::string name(reinterpret_cast<const char*>(text));
            if (name.empty() || name.back() != ':' || name.front() == ':') continue;
            name.pop_back();
            name = utils::ToLowerAscii(std::move(name));
            if (!utils::IsValidHttpHeaderName(name)) continue;
            const int length = lws_hdr_total_length(wsi, token);
            if (length <= 0) continue;
            // Ambiguous credential/framing duplicates must not reach business.
            if ((name == "authorization" || name == "content-length") &&
                lws_hdr_fragment_length(wsi, token, 1) > 0) return false;
            if (static_cast<std::size_t>(length) > options_.max_header_bytes) return false;
            std::vector<char> buffer(static_cast<std::size_t>(length) + 1);
            const int copied = lws_hdr_copy(wsi, buffer.data(), static_cast<int>(buffer.size()), token);
            if (copied < 0) return false;
            request.headers.emplace(std::move(name), std::string(buffer.data(), copied));
        }
#if defined(LWS_WITH_CUSTOM_HEADERS)
        struct CustomHeaders { lws* wsi; HttpRequest* request; bool valid = true; } custom{wsi, &request};
        if (lws_hdr_custom_name_foreach(wsi, [](const char* name, int length, void* opaque) {
            auto& custom = *static_cast<CustomHeaders*>(opaque);
            // Exceptions cannot unwind through this nested C callback either.
            try
            {
                const int size = lws_hdr_custom_length(custom.wsi, name, length);
                if (size < 0) { custom.valid = false; return; }
                std::vector<char> value(static_cast<std::size_t>(size) + 1);
                const int copied = lws_hdr_custom_copy(custom.wsi, value.data(), static_cast<int>(value.size()), name, length);
                std::string key(name, length);
                if (!key.empty() && key.back() == ':') key.pop_back();
                key = utils::ToLowerAscii(std::move(key));
                if (copied < 0 || !utils::IsValidHttpHeaderName(key) ||
                    !custom.request->headers.emplace(std::move(key), std::string(value.data(), copied)).second)
                    custom.valid = false;
            }
            catch (...) { custom.valid = false; }
        }, &custom) < 0 || !custom.valid) return false;
#endif
        return true;
    }

    int Begin(lws* wsi)
    {
        if (stopping_) return -1;
        auto existing = connections_.find(wsi);
        if (existing != connections_.end() && existing->second.request_id) return -1;
        auto& connection = connections_[wsi];
        connection = {};
        connection.request_id = ++next_request_id_;
        sockets_.emplace(connection.request_id, wsi);
        {
            std::lock_guard<std::mutex> lock(mail_mutex_);
            slots_.emplace(connection.request_id, Slot{});
        }
        lws_set_timer_usecs(wsi, static_cast<lws_usec_t>(options_.request_timeout_seconds) * LWS_USEC_PER_SEC);
        char* uri = nullptr;
        int length = 0;
        const int method = lws_http_get_uri_and_method(wsi, &uri, &length);
        static constexpr const char* methods[] = {"GET", "POST", "OPTIONS", "PUT", "PATCH", "DELETE", "CONNECT", "HEAD"};
        if (method < 0 || method >= 8 || !uri || length < 0) return Error(wsi, connection, 400);
        connection.request.method = methods[method];
        connection.head = method == LWSHUMETH_HEAD;
        connection.request.path.assign(uri, length);
        if (!ReadHeaders(wsi, connection.request)) return Error(wsi, connection, 400);
        const auto& headers = connection.request.headers;
        // This bounded JSON API adapter accepts Content-Length framing. Reject
        // transfer encodings and protocol upgrades rather than misframe them.
        if (headers.count("transfer-encoding") || headers.count("upgrade") || method == LWSHUMETH_CONNECT)
            return Error(wsi, connection, 400);
        const auto content_length = headers.find("content-length");
        if (content_length != headers.end())
        {
            const auto& value = content_length->second;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), connection.expected_body);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
                return Error(wsi, connection, 400);
        }
        if (connection.expected_body > options_.max_body_bytes) return Error(wsi, connection, 413);
        if (!connection.expected_body) Dispatch(wsi, connection);
        return stopping_ ? -1 : 0;
    }

    int Complete(lws* wsi, Connection& connection)
    {
        const bool close = connection.close_after_response;
        Retire(connection.request_id);
        connection = {};
        if (close) return -1;
        lws_set_timer_usecs(wsi, static_cast<lws_usec_t>(options_.idle_timeout_seconds) * LWS_USEC_PER_SEC);
        return lws_http_transaction_completed(wsi) ? -1 : 0;
    }

    int Write(lws* wsi)
    {
        auto it = connections_.find(wsi);
        if (it == connections_.end() || stopping_) return -1;
        auto& connection = it->second;
        if (!connection.has_response) return 0;
        auto& response = connection.response;
        const bool no_body = connection.head || response.status == 204 || response.status == 304;
        if (!connection.headers_sent)
        {
            std::vector<unsigned char> buffer(LWS_PRE + options_.max_header_bytes + 1024);
            auto* start = buffer.data() + LWS_PRE;
            auto* p = start;
            auto* end = buffer.data() + buffer.size();
            const auto type = response.headers.find("content-type");
            const char* content_type = type == response.headers.end() ? "application/octet-stream" : type->second.c_str();
            const auto size = (response.status == 204 || response.status == 304)
                ? LWS_ILLEGAL_HTTP_CONTENT_LEN : static_cast<lws_filepos_t>(response.body.size());
            if (lws_add_http_common_headers(wsi, response.status, content_type, size, &p, end)) return -1;
            for (const auto& header : response.headers)
            {
                if (header.first == "content-type" || header.first == "content-length" ||
                    header.first == "transfer-encoding" || header.first == "connection") continue;
                const auto name = header.first + ':';
                if (lws_add_http_header_by_name(wsi, reinterpret_cast<const unsigned char*>(name.c_str()),
                    reinterpret_cast<const unsigned char*>(header.second.data()), static_cast<int>(header.second.size()), &p, end))
                    return -1;
            }
            if (connection.close_after_response && lws_add_http_header_by_token(wsi, WSI_TOKEN_CONNECTION,
                reinterpret_cast<const unsigned char*>("close"), 5, &p, end)) return -1;
            if (lws_finalize_write_http_header(wsi, start, &p, end)) return -1;
            connection.headers_sent = true;
            if (no_body || response.body.empty()) return Complete(wsi, connection);
            lws_callback_on_writable(wsi);
            return 0;
        }
        const auto count = std::min(kWriteChunk, response.body.size() - connection.offset);
        std::vector<unsigned char> buffer(LWS_PRE + count);
        std::copy_n(response.body.data() + connection.offset, count, buffer.data() + LWS_PRE);
        const bool final = connection.offset + count == response.body.size();
        const int written = lws_write(wsi, buffer.data() + LWS_PRE, count, final ? LWS_WRITE_HTTP_FINAL : LWS_WRITE_HTTP);
        // lws buffers a short kernel write internally. A short API return means
        // failure, not permission to call lws_write again in this callback.
        if (written < static_cast<int>(count)) return -1;
        connection.offset += count;
        if (final) return Complete(wsi, connection);
        lws_callback_on_writable(wsi);
        return 0;
    }

    static int Callback(lws* wsi, lws_callback_reasons reason, void* user, void* in, std::size_t len)
    {
        if (!wsi) return 0;
        auto* self = static_cast<Impl*>(lws_context_user(lws_get_context(wsi)));
        if (!self) return 0;
        try { return self->Handle(wsi, reason, user, in, len); }
        catch (...) { lwsl_err("Packetia: HTTP callback failed; closing connection\n"); return -1; }
    }

    int Handle(lws* wsi, lws_callback_reasons reason, void* user, void* in, std::size_t len)
    {
        switch (reason)
        {
        case LWS_CALLBACK_FILTER_NETWORK_CONNECTION:
            return stopping_ || network_connections_.size() >= options_.max_connections ? -1 : 0;
        case LWS_CALLBACK_SERVER_NEW_CLIENT_INSTANTIATED:
            network_connections_.insert(wsi);
            lws_set_timer_usecs(wsi, static_cast<lws_usec_t>(options_.request_timeout_seconds) * LWS_USEC_PER_SEC);
            return 0;
        case LWS_CALLBACK_FILTER_PROTOCOL_CONNECTION:
            return -1; // This listener does not accept WebSocket upgrades.
        case LWS_CALLBACK_HTTP:
            return Begin(wsi);
        case LWS_CALLBACK_HTTP_BODY:
        {
            auto it = connections_.find(wsi);
            if (it == connections_.end() || stopping_) return -1;
            auto& connection = it->second;
            if (connection.dispatched) return 0; // Error response awaiting writable.
            if (len > connection.expected_body - connection.request.body.size()) return Error(wsi, connection, 400);
            connection.request.body.append(static_cast<const char*>(in), len);
            return 0;
        }
        case LWS_CALLBACK_HTTP_BODY_COMPLETION:
        {
            auto it = connections_.find(wsi);
            if (it == connections_.end() || stopping_) return -1;
            auto& connection = it->second;
            if (!connection.dispatched)
            {
                if (connection.request.body.size() != connection.expected_body) return Error(wsi, connection, 400);
                Dispatch(wsi, connection);
            }
            return stopping_ ? -1 : 0;
        }
        case LWS_CALLBACK_HTTP_WRITEABLE:
            return Write(wsi);
        case LWS_CALLBACK_TIMER:
            return -1; // Absolute request deadline, or idle keep-alive deadline.
        case LWS_CALLBACK_CLOSED_HTTP:
        case LWS_CALLBACK_WSI_DESTROY:
            Drop(wsi);
            return 0;
        default:
            return lws_callback_http_dummy(wsi, reason, user, in, len);
        }
    }

    EventLoop* loop_;
    std::shared_ptr<TaskScheduler> scheduler_;
    std::mutex owner_mutex_;
    std::recursive_mutex context_mutex_;
    lws_context* context_ = nullptr;
    lws_protocols protocols_[2]{};
    HttpServerOptions options_;
    RequestHandler handler_;
    TimeId timer_ = 0;
    std::atomic<bool> stopping_{true};
    std::atomic<bool> service_posted_{false};
    std::atomic<std::uint64_t> generation_{0};
    bool servicing_ = false;
    bool destroying_ = false;
    std::uint64_t next_request_id_ = 0;
    std::unordered_set<lws*> network_connections_;
    std::unordered_map<lws*, Connection> connections_;
    std::unordered_map<std::uint64_t, lws*> sockets_;
    std::mutex mail_mutex_;
    bool accepting_replies_ = false;
    std::size_t response_bytes_ = 0;
    std::unordered_map<std::uint64_t, Slot> slots_;
    std::unordered_set<std::uint64_t> ready_;
};
#else
class HttpServer::Impl
{
public:
    Impl(EventLoop*, std::shared_ptr<TaskScheduler>) {}
    bool Start(const HttpServerOptions& options, RequestHandler)
    {
        if (!options.enabled) return true;
        LOG_ERROR("[HttpServer] libwebsockets backend is disabled");
        return false;
    }
    void Stop() {}
};
#endif

HttpServer::HttpServer(EventLoop* loop)
    : impl_(std::make_shared<Impl>(loop, nullptr)) {}
HttpServer::HttpServer(std::shared_ptr<TaskScheduler> scheduler)
    : impl_(std::make_shared<Impl>(nullptr, std::move(scheduler))) {}
HttpServer::~HttpServer() { Stop(); }
bool HttpServer::Start(const HttpServerOptions& options, RequestHandler handler)
{
    try { return impl_->Start(options, std::move(handler)); }
    catch (...) { Stop(); return false; }
}
void HttpServer::Stop() noexcept
{
    try { impl_->Stop(); }
    catch (...) { /* Never unwind through application shutdown. */ }
}
}
