#ifndef PACKETIA_SERVER_API_BUSINESS_ACCESS_POLICY_H_
#define PACKETIA_SERVER_API_BUSINESS_ACCESS_POLICY_H_

#include "HttpMessage.h"

#include <optional>
#include <string_view>

namespace service::account { struct AccountIdentity; }

namespace server::api
{

class BusinessAccessPolicy
{
public:
    using Rejection = std::optional<protocol::http::HttpResponse>;

    virtual ~BusinessAccessPolicy() = default;

    virtual Rejection CheckLogin(std::string_view login) const;

    virtual Rejection CheckCreateRoom(const service::account::AccountIdentity& identity) const;
    virtual Rejection CheckRoomAdmission(const service::account::AccountIdentity& identity, std::string_view room_id) const;
};

}

#endif // PACKETIA_SERVER_API_BUSINESS_ACCESS_POLICY_H_
