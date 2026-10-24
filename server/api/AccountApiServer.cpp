#include "ApiServer.h"
#include "AccountController.h"
#include "AccountOptions.h"
#include <utility>
#include <algorithm>

namespace server::api
{
namespace
{
ApiRouter AccountRoutes(const std::shared_ptr<service::account::AccountService>& accounts, size_t limit)
{
    ApiRouter::Builder routes;
    AccountController(accounts).RegisterRoutes(routes);
    return routes.Build(std::max<size_t>(1, limit));
}
}
// Compatibility constructor is linked only by account API consumers.
ApiServer::ApiServer(protocol::http::HttpServerOptions http, service::account::AccountOptions options,
                     std::shared_ptr<service::account::AccountService> accounts,
                     std::shared_ptr<protocol::http::IHttpServer> transport)
    : ApiServer(http, AccountRoutes(accounts, http.max_body_bytes), std::move(transport),
                options.worker_count, options.max_pending_requests)
{ configuration_valid_ = options.enabled && options.IsValid() && static_cast<bool>(accounts); }
}
