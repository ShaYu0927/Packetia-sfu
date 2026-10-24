#include "ApiServer.h"
#include "ApiResponse.h"
#include "ApiRouter.h"
#include "logger.h"

#include <atomic>
#include <mutex>
#include <utility>

namespace server::api
{
namespace
{

std::atomic<std::uint64_t> next_pool_id{0};

protocol::http::HttpResponse Unavailable()
{
    return ErrorResponse(503, "unavailable", "API unavailable.");
}

void RespondSafely(const protocol::http::IHttpServer::Respond& respond,
                   protocol::http::HttpResponse response) noexcept
{
    try { if (respond) respond(std::move(response)); }
    catch (...)
    {
        try { LOG_ERROR("[ApiServer] transport response callback failed"); }
        catch (...) {} 
    }
}

}

struct ApiServer::State
{
    State(ApiRouter routes,
          std::size_t body_limit, std::size_t pending_limit, std::string name)
        : router(std::move(routes)), max_body_bytes(body_limit),
          max_pending(pending_limit), pool_name(std::move(name)) {}

    const ApiRouter router;
    const std::size_t max_body_bytes;
    const std::size_t max_pending;
    const std::string pool_name;
    std::mutex mutex;
    bool accepting = true;
    std::size_t pending = 0;
    std::uint64_t next_key = 0;

    static void Dispatch(const std::weak_ptr<State>& weak, protocol::http::HttpRequest request, protocol::http::IHttpServer::Respond respond)
    {
        const auto state = weak.lock();
        if (!state) { RespondSafely(respond, Unavailable()); return; }
        if (request.body.size() > state->max_body_bytes)
        {
            RespondSafely(respond, ErrorResponse(413, "body_too_large", "Request body too large."));
            return;
        }

        bool admitted = false;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->accepting && state->pending < state->max_pending)
            {
                ++state->pending;
                try
                {
                    admitted = WorkerService::post_fn(state->pool_name, state->next_key++,
                        [state, request = std::move(request), respond]() mutable
                        {
                            struct Completion
                            {
                                std::shared_ptr<State> state;
                                ~Completion()
                                {
                                    std::lock_guard<std::mutex> lock(state->mutex);
                                    --state->pending;
                                }
                            } completion{state};
                            try
                            {
                                RespondSafely(respond, state->router.Dispatch(request));
                            }
                            catch (...)
                            {
                                try { RespondSafely(respond, Unavailable()); }
                                catch (...) {} // Allocation failure must not escape a worker.
                            }
                        }) == 0;
                }
                catch (...) { admitted = false; }
                if (!admitted) --state->pending;
            }
        }
        if (!admitted) RespondSafely(respond, Unavailable());
    }
};

ApiServer::~ApiServer()
{
    Stop();
}

ApiServer::ApiServer(protocol::http::HttpServerOptions http_options, ApiRouter router,
                     std::shared_ptr<protocol::http::IHttpServer> transport,
                     uint32_t worker_count, std::size_t max_pending_requests)
    : http_options_(std::move(http_options)), worker_count_(worker_count), max_pending_requests_(max_pending_requests),
      transport_(std::move(transport)), pool_name_("api_" + std::to_string(next_pool_id.fetch_add(1))),
      routes_(std::make_shared<const ApiRouter>(std::move(router)))
{
    workers_.AddFunction({pool_name_, worker_count, max_pending_requests,
                          ShardedWorkerPool::DropPolicy::DropTail});
}

bool ApiServer::Start()
{
    if (started_ || !http_options_.enabled) return true;
    if (!configuration_valid_ || !routes_ || !worker_count_ || worker_count_ > 64 || !max_pending_requests_ || max_pending_requests_ > 65536 || !transport_ ||
        !http_options_.IsValid())
    {
        LOG_ERROR("[ApiServer] invalid API configuration");
        return false;
    }

    try
    {
        if (workers_.Start() != 0) return false;
        state_ = std::make_shared<State>(*routes_, http_options_.max_body_bytes, max_pending_requests_, pool_name_);
        const std::weak_ptr<State> weak = state_;
        if (!transport_->Start(http_options_, [weak](auto request, auto respond)
            {
                try { State::Dispatch(weak, std::move(request), respond); }
                catch (...)
                {
                    try { RespondSafely(respond, Unavailable()); }
                    catch (...) {} 
                }
            }))
        {
            Stop();
            return false;
        }
        started_ = true;
        return true;
    }
    catch (...)
    {
        LOG_ERROR("[ApiServer] API startup failed");
        Stop();
        return false;
    }
}

void ApiServer::Stop() noexcept
{
    if (state_)
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->accepting = false;
    }
    if (transport_ && state_) transport_->Stop();
    workers_.Stop(true);
    state_.reset();
    started_ = false;
}

}
