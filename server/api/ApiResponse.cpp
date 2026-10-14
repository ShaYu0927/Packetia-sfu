#include "ApiResponse.h"

namespace server::api
{

protocol::http::HttpResponse JsonResponse(std::uint16_t status, nlohmann::json body)
{
    return {status, body.dump(), {{"content-type", "application/json; charset=utf-8"},
                                 {"cache-control", "no-store"},
                                 {"x-content-type-options", "nosniff"}}};
}

protocol::http::HttpResponse ErrorResponse(std::uint16_t status, const char* code, const char* message)
{
    auto response = JsonResponse(status, {{"error", {{"code", code}, {"message", message}}}});
    if (status == 401) response.headers["www-authenticate"] = "Bearer";
    return response;
}

}
