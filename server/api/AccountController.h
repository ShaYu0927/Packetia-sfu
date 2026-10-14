#ifndef PACKETIA_SERVER_API_ACCOUNT_CONTROLLER_H_
#define PACKETIA_SERVER_API_ACCOUNT_CONTROLLER_H_

#include "ApiRouter.h"

#include <memory>

namespace service::account { class AccountService; }

namespace server::api
{

class AccountController final
{
public:
    explicit AccountController(std::shared_ptr<service::account::AccountService> accounts);

    void RegisterRoutes(ApiRouter::Builder& router) const;

private:
    protocol::http::HttpResponse RegisterAccount(const ApiRequest& request) const;
    protocol::http::HttpResponse Login(const ApiRequest& request) const;

    const std::shared_ptr<service::account::AccountService> accounts_;
};

}

#endif // PACKETIA_SERVER_API_ACCOUNT_CONTROLLER_H_
