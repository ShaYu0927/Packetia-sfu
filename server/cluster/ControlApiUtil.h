#pragma once
#include "ApiRouter.h"
#include "ApiResponse.h"
#include "ClusterTypes.h"
#include <openssl/crypto.h>
#include <algorithm>
#include <cctype>
#include <set>
namespace server::control_api
{
using namespace server::api;
using service::cluster::Failure;
using Json = nlohmann::json;
using Response = protocol::http::HttpResponse;
inline std::string Bearer(const ApiRequest& request)
{
    const auto it = request.http.headers.find("authorization");
    if (it == request.http.headers.end() || it->second.size() < 8 || it->second.size() > 4103) return {};
    auto prefix = it->second.substr(0, 7);
    std::transform(prefix.begin(), prefix.end(), prefix.begin(), [](unsigned char c) { return std::tolower(c); });
    return prefix == "bearer " ? it->second.substr(7) : "";
}
inline std::string String(const Json& json, const char* name, std::size_t limit = 128)
{
    const auto it = json.find(name);
    if (it == json.end() || !it->is_string()) throw Failure(400, "invalid_request");
    auto value = it->get<std::string>();
    if (value.empty() || value.size() > limit) throw Failure(400, "invalid_request");
    return value;
}
inline int Number(const Json& json, const char* name, int low, int high)
{
    const auto it = json.find(name);
    if (it == json.end() || !it->is_number_integer()) throw Failure(400, "invalid_request");
    if (*it < low || *it > high) throw Failure(400, "invalid_request");
    return it->get<int>();
}
inline void Fields(const Json& json, std::initializer_list<const char*> names)
{
    std::set<std::string> allowed(names.begin(), names.end());
    for (auto it = json.begin(); it != json.end(); ++it)
        if (!allowed.count(it.key())) throw Failure(400, "unknown_field");
}
inline ApiRouter::Handler Guard(ApiRouter::Handler handler)
{
    return [handler = std::move(handler)](const ApiRequest& request) -> Response {
        try { return handler(request); }
        catch (const Failure& error) { return ErrorResponse(error.status, error.code, "Cluster operation rejected."); }
    };
}
inline bool MatchesCredential(const std::string& provided, const std::string& expected)
{
    return provided.size() == expected.size() && !expected.empty() &&
        CRYPTO_memcmp(provided.data(), expected.data(), expected.size()) == 0;
}
}
