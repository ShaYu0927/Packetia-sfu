#include "ApiServer.h"
#include "AccountController.h"
#include "log/logger.h"

#include <atomic>
#include <mutex>
#include <utility>

namespace server::api
{
namespace
{

std::atomic<std::uint64_t> next_pool_id{0};

network::http::HttpResponse Unavailable()
{
    return {503, R"({"error":{"code":"unavailable","message":"Account API unavailable."}})",
            {{"content-type", "application/json; charset=utf-8"}, {"cache-control", "no-store"}}};
}

void RespondSafely(const network::http::HttpServer::Respond& respond,
                   network::http::HttpResponse response) noexcept
{
    try { if (respond) respond(std::move(response)); }
    catch (...)
    {
        try { LOG_ERROR("[ApiServer] transport response callback failed"); }
        catch (...) {} // Logging must not break the no-throw response boundary.
    }
}

}

struct ApiServer::State
{
    State(std::shared_ptr<service::account::AccountService> accounts,
          std::size_t body_limit, std::size_t pending_limit, std::string name)
        : controller(std::move(accounts), body_limit), max_body_bytes(body_limit),
          max_pending(pending_limit), pool_name(std::move(name)) {}

    AccountController controller;
    const std::size_t max_body_bytes;
    const std::size_t max_pending;
    const std::string pool_name;
    std::mutex mutex;
    bool accepting = true;
    std::size_t pending = 0;
    std::uint64_t next_key = 0;

    static void Dispatch(const std::weak_ptr<State>& weak,
                         network::http::HttpRequest request,
                         network::http::HttpServer::Respond respond)
    {
        const auto state = weak.lock();
        if (!state) { RespondSafely(respond, Unavailable()); return; }
        if (request.body.size() > state->max_body_bytes)
        {
            RespondSafely(respond,
                {413, R"({"error":{"code":"body_too_large","message":"Request body too large."}})",
                 {{"content-type", "application/json; charset=utf-8"}, {"cache-control", "no-store"}}});
            return;
        }

        bool admitted = false;
        {
            // Serialize admission with Stop; shutdown cannot remove the pool
            // between checking acceptance and posting the owned request.
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
                                // The HTTP adapter returns this to its I/O owner.
                                RespondSafely(respond, state->controller.Handle(request));
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

ApiServer::ApiServer(network::http::HttpOptions http_options,
                     service::account::AccountOptions account_options,
                     std::shared_ptr<service::account::AccountService> accounts,
                     std::shared_ptr<network::http::HttpServer> transport)
    : http_options_(std::move(http_options)), account_options_(std::move(account_options)),
      accounts_(std::move(accounts)), transport_(std::move(transport)),
      pool_name_("account_api_" + std::to_string(next_pool_id.fetch_add(1)))
{
    workers_.AddFunction({pool_name_, account_options_.worker_count,
                          account_options_.max_pending_requests,
                          ShardedWorkerPool::DropPolicy::DropTail});
}

ApiServer::~ApiServer()
{
    Stop();
}

bool ApiServer::Start()
{
    if (started_ || !http_options_.enabled) return true;
    if (!account_options_.enabled || !account_options_.IsValid() || !accounts_ || !transport_ ||
        http_options_.listen_ip.empty() ||
        http_options_.listen_port == 0 || http_options_.max_body_bytes == 0 ||
        http_options_.max_body_bytes > 1024 * 1024)
    {
        LOG_ERROR("[ApiServer] invalid or missing account API configuration");
        return false;
    }

    try
    {
        if (workers_.Start() != 0) return false;
        state_ = std::make_shared<State>(accounts_, http_options_.max_body_bytes,
                                          account_options_.max_pending_requests, pool_name_);
        const std::weak_ptr<State> weak = state_;
        if (!transport_->Start(http_options_, [weak](auto request, auto respond)
            {
                try { State::Dispatch(weak, std::move(request), respond); }
                catch (...)
                {
                    try { RespondSafely(respond, Unavailable()); }
                    catch (...) {} // Do not unwind through the network callback.
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
