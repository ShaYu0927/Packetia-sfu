#include "ApiRouter.h"
#include "ApiResponse.h"
#include "StringUtil.h"

#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace server::api
{
namespace
{

bool IsJson(const protocol::http::HttpRequest& request)
{
    const auto found = request.headers.find("content-type");
    if (found == request.headers.end()) return false;
    std::string type = found->second.substr(0, found->second.find(';'));
    while (!type.empty() && (type.back() == ' ' || type.back() == '\t')) type.pop_back();
    return utils::ToLowerAscii(std::move(type)) == "application/json";
}

nlohmann::json ParseObject(const std::string& body)
{
    using Json = nlohmann::json;
    std::vector<std::set<std::string>> keys;
    auto callback = [&](int depth, Json::parse_event_t event, Json& parsed)
    {
        if (depth > 8) throw std::invalid_argument("JSON nesting");
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        else if (event == Json::parse_event_t::key)
        {
            if (keys.empty() || !keys.back().insert(parsed.get<std::string>()).second)
                throw std::invalid_argument("duplicate JSON key");
        }
        else if (event == Json::parse_event_t::object_end) keys.pop_back();
        return true;
    };
    auto value = Json::parse(body, callback);
    if (!value.is_object()) throw std::invalid_argument("JSON object required");
    return value;
}

}

ApiRouter::ApiRouter(Routes routes, std::size_t max_body_bytes)
    : routes_(std::move(routes)), max_body_bytes_(max_body_bytes) {}

ApiRouter::Builder& ApiRouter::Builder::Add(std::string method, std::string path,
                                           Handler handler, BodyMode body_mode)
{
    if (method.empty() || path.empty() || path.front() != '/' || !handler ||
        path.find_first_of("?#") != std::string::npos)
        throw std::invalid_argument("invalid API method, path or handler");
    for (char c : method)
        if (!utils::IsTokenCharacter(c) || (c >= 'a' && c <= 'z'))
            throw std::invalid_argument("API method must be an uppercase token");
    for (unsigned char c : path)
        if (c <= 32 || c == 127) throw std::invalid_argument("invalid API path");
    auto& methods = routes_[path];
    if (!methods.emplace(std::move(method), Route{std::move(handler), body_mode}).second)
        throw std::logic_error("duplicate API method and path");
    return *this;
}

ApiRouter::Builder& ApiRouter::Builder::Get(std::string path, Handler handler)
{
    return Add("GET", std::move(path), std::move(handler), BodyMode::Empty);
}

ApiRouter::Builder& ApiRouter::Builder::Post(std::string path, Handler handler, BodyMode body_mode)
{
    return Add("POST", std::move(path), std::move(handler), body_mode);
}

ApiRouter::Builder& ApiRouter::Builder::PostJson(std::string path, Handler handler)
{
    return Post(std::move(path), std::move(handler), BodyMode::Json);
}

ApiRouter::Builder& ApiRouter::Builder::Delete(std::string path, Handler handler)
{
    return Add("DELETE", std::move(path), std::move(handler), BodyMode::Empty);
}

ApiRouter ApiRouter::Builder::Build(std::size_t max_body_bytes) const
{
    if (!max_body_bytes) throw std::invalid_argument("API body limit must be positive");
    return ApiRouter(routes_, max_body_bytes);
}

protocol::http::HttpResponse ApiRouter::Dispatch(const protocol::http::HttpRequest& request) const
{
    if (request.body.size() > max_body_bytes_)
        return ErrorResponse(413, "body_too_large", "Request body too large.");
    const auto path = routes_.find(request.path);
    if (path == routes_.end()) return ErrorResponse(404, "not_found", "API route not found.");
    const auto route = path->second.find(request.method);
    if (route == path->second.end())
    {
        auto response = ErrorResponse(405, "method_not_allowed", "Method not allowed.");
        std::string allowed;
        for (const auto& method : path->second)
        {
            if (!allowed.empty()) allowed += ", ";
            allowed += method.first;
        }
        response.headers["allow"] = std::move(allowed);
        return response;
    }

    try
    {
        nlohmann::json body;
        if (route->second.body_mode == BodyMode::Empty && !request.body.empty())
            return ErrorResponse(400, "invalid_request", "Invalid request.");
        if (route->second.body_mode == BodyMode::Json)
        {
            if (!IsJson(request)) return ErrorResponse(415, "unsupported_media_type", "JSON body required.");
            try { body = ParseObject(request.body); }
            catch (const nlohmann::json::exception&) { return ErrorResponse(400, "invalid_request", "Invalid request."); }
            catch (const std::invalid_argument&) { return ErrorResponse(400, "invalid_request", "Invalid request."); }
        }
        return route->second.handler({request, body});
    }
    catch (...)
    {
        // Never expose storage details, exception messages or credentials.
        return ErrorResponse(500, "internal_error", "API operation failed.");
    }
}

}
