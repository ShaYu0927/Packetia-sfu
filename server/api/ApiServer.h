#ifndef PACKETIA_SERVER_API_SERVER_H_
#define PACKETIA_SERVER_API_SERVER_H_

#include "network/http/HttpServer.h"
#include "service/Account/AccountOptions.h"
#include "utils/WorkerRegistry.h"

#include <memory>

namespace service::account { class AccountService; }

namespace server::api
{

// Owns API execution, not the account store. The application starts AccountService
// first and stops it only after ApiServer::Stop has drained accepted work.
class ApiServer final
{
public:
    ApiServer(network::http::HttpOptions http_options,
              service::account::AccountOptions account_options,
              std::shared_ptr<service::account::AccountService> accounts,
              std::shared_ptr<network::http::HttpServer> transport);
    ~ApiServer();

    ApiServer(const ApiServer&) = delete;
    ApiServer& operator=(const ApiServer&) = delete;

    // Lifecycle calls are serialized by ServerApp, never called by API workers.
    bool Start();
    void Stop() noexcept;

private:
    struct State;

    const network::http::HttpOptions http_options_;
    const service::account::AccountOptions account_options_;
    const std::shared_ptr<service::account::AccountService> accounts_;
    const std::shared_ptr<network::http::HttpServer> transport_;
    const std::string pool_name_;
    WorkerRegistry workers_;
    std::shared_ptr<State> state_;
    bool started_ = false;
};

}

#endif // PACKETIA_SERVER_API_SERVER_H_
