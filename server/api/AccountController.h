#ifndef PACKETIA_SERVER_API_ACCOUNT_CONTROLLER_H_
#define PACKETIA_SERVER_API_ACCOUNT_CONTROLLER_H_

#include "network/http/HttpTypes.h"

#include <cstddef>
#include <memory>

namespace service::account { class AccountService; }

namespace server::api
{

class AccountController final
{
public:
    explicit AccountController(std::shared_ptr<service::account::AccountService> accounts,
                               std::size_t max_body_bytes = 8192);

    // Synchronous business boundary. ApiServer invokes it on its account pool.
    // It never stores connection objects or room/media authorization state.
    network::http::HttpResponse Handle(const network::http::HttpRequest& request) const;

private:
    const std::shared_ptr<service::account::AccountService> accounts_;
    const std::size_t max_body_bytes_;
};

}

#endif // PACKETIA_SERVER_API_ACCOUNT_CONTROLLER_H_
