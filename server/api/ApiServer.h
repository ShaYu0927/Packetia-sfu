#ifndef PACKETIA_SERVER_API_SERVER_H_
#define PACKETIA_SERVER_API_SERVER_H_

#include "protocol/http/IHttpServer.h"
#include "service/Account/AccountOptions.h"
#include "utils/WorkerRegistry.h"

#include <memory>

namespace service::account { class AccountService; }

namespace server::api
{


class ApiServer final
{
public:
    ApiServer(protocol::http::HttpServerOptions http_options,
              service::account::AccountOptions account_options,
              std::shared_ptr<service::account::AccountService> accounts,
              std::shared_ptr<protocol::http::IHttpServer> transport);
    ~ApiServer();

    ApiServer(const ApiServer&) = delete;
    ApiServer& operator=(const ApiServer&) = delete;

    bool Start();
    void Stop() noexcept;

private:
    struct State;

    const protocol::http::HttpServerOptions http_options_;
    const service::account::AccountOptions account_options_;
    const std::shared_ptr<service::account::AccountService> accounts_;
    const std::shared_ptr<protocol::http::IHttpServer> transport_;
    const std::string pool_name_;
    WorkerRegistry workers_;
    std::shared_ptr<State> state_;
    bool started_ = false;
};

}

#endif // PACKETIA_SERVER_API_SERVER_H_
