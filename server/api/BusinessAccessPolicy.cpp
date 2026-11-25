#include "BusinessAccessPolicy.h"
#include "AccountPolicy.h"
#include "ApiResponse.h"
#include "AccountTypes.h"

namespace server::api
{

BusinessAccessPolicy::Rejection BusinessAccessPolicy::CheckLogin(std::string_view login) const
{
    std::string normalized_login;
    if (!service::account::NormalizeLogin(login, normalized_login))
        return ErrorResponse(
            400,
            "invalid_request",
            "Invalid login."
        );

    if(normalized_login == "banned_user")
    {
        return ErrorResponse(
            403,
            "login_restricted",
            "Login is restricted."
        );
    }

    return std::nullopt;
}

BusinessAccessPolicy::Rejection BusinessAccessPolicy::CheckCreateRoom(const service::account::AccountIdentity& identity) const
{
    if(identity.user_id == "restricted_user")
    {
        return ErrorResponse(
            403,
            "create_room_restricted",
            "You are not allowed to create rooms."
        );
    }

    return std::nullopt;
}

BusinessAccessPolicy::Rejection BusinessAccessPolicy::CheckRoomAdmission(const service::account::AccountIdentity& identity, std::string_view room_id) const
{
    /* 示例 */
    if(identity.user_id == "restricted_user")
    {
        return ErrorResponse(
            403,
            "room_admission_restricted",
            "You are not allowed to join this room."
        );
    }
    return std::nullopt;
}

}
