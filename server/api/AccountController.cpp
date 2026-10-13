#include "AccountController.h"
#include "service/Account/AccountService.h"
#include "third/nlohmann/json.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace server::api
{
namespace
{

using Json = nlohmann::json;
using network::http::HttpRequest;
using network::http::HttpResponse;
using service::account::AccountError;
constexpr std::size_t kMaxLoginBytes = 256;
constexpr std::size_t kMaxPasswordBytes = 4096;
constexpr std::size_t kMaxSessionTokenBytes = 4096;

HttpResponse JsonResponse(std::uint16_t status, Json body)
{
    return {status, body.dump(), {{"content-type", "application/json; charset=utf-8"},
                                 {"cache-control", "no-store"},
                                 {"x-content-type-options", "nosniff"}}};
}

HttpResponse Error(std::uint16_t status, const char* code, const char* message)
{
    auto response = JsonResponse(status, {{"error", {{"code", code}, {"message", message}}}});
    if (status == 401) response.headers["www-authenticate"] = "Bearer";
    return response;
}

HttpResponse AccountFailure(AccountError error)
{
    switch (error)
    {
    case AccountError::InvalidArgument:
        return Error(400, "invalid_request", "Invalid request.");
    case AccountError::Unauthenticated:
    case AccountError::InvalidCredentials:
    case AccountError::NotFound:
    case AccountError::UserDisabled:
    case AccountError::SessionExpired:
    case AccountError::SessionRevoked:
        // Do not reveal whether an account exists, is disabled or has sessions.
        return Error(401, "unauthenticated", "Authentication required or credentials invalid.");
    case AccountError::AlreadyExists:
        return Error(409, "already_exists", "Account cannot be created.");
    case AccountError::NotImplemented:
        return Error(501, "not_implemented", "Account operation is not implemented.");
    case AccountError::NotConfigured:
    case AccountError::Unavailable:
        return Error(503, "unavailable", "Account service unavailable.");
    case AccountError::None: // Success without a value is an internal error.
    case AccountError::StorageError:
    case AccountError::InternalError:
        return Error(500, "internal_error", "Account operation failed.");
    }
    return Error(500, "internal_error", "Account operation failed.");
}

char LowerAscii(char ch)
{
    return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch + ('a' - 'A')) : ch;
}

bool IsJson(const HttpRequest& request)
{
    const auto found = request.headers.find("content-type");
    if (found == request.headers.end()) return false;
    std::string type = found->second.substr(0, found->second.find(';'));
    while (!type.empty() && (type.back() == ' ' || type.back() == '\t')) type.pop_back();
    std::transform(type.begin(), type.end(), type.begin(), LowerAscii);
    return type == "application/json";
}

Json ParseObject(const std::string& body)
{
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

bool ReadString(const Json& body, const char* key, std::size_t max_bytes,
                std::string& value, bool required = true)
{
    const auto field = body.find(key);
    if (field == body.end()) return !required;
    if (!field->is_string()) return false;
    value = field->get<std::string>();
    return value.size() <= max_bytes && (!required || !value.empty()) &&
           value.find('\0') == std::string::npos;
}

bool OnlyFields(const Json& body, std::initializer_list<std::string_view> allowed)
{
    for (auto it = body.begin(); it != body.end(); ++it)
        if (std::find(allowed.begin(), allowed.end(), it.key()) == allowed.end()) return false;
    return true;
}

std::string_view BearerToken(const HttpRequest& request)
{
    const auto found = request.headers.find("authorization");
    if (found == request.headers.end()) return {};
    const std::string_view value(found->second);
    constexpr std::string_view prefix = "bearer ";
    if (value.size() <= prefix.size() || value.size() > prefix.size() + kMaxSessionTokenBytes)
        return {};
    for (std::size_t i = 0; i < prefix.size(); ++i)
        if (LowerAscii(value[i]) != prefix[i]) return {};
    auto token = value.substr(prefix.size());
    for (const char ch : token)
    {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '.' || ch == '_' ||
            ch == '~' || ch == '+' || ch == '/' || ch == '=') continue;
        return {};
    }
    return token;
}

Json IdentityJson(const service::account::AccountIdentity& identity)
{
    return {{"user_id", identity.user_id}, {"session_id", identity.session_id},
            {"display_name", identity.display_name}};
}

HttpResponse SessionResponse(const service::account::LoginResult& result)
{
    return JsonResponse(200, {{"identity", IdentityJson(result.identity)},
                              {"session_token", result.session_token},
                              {"expires_at_unix_seconds", result.expires_at_unix_seconds}});
}

}

AccountController::AccountController(std::shared_ptr<service::account::AccountService> accounts,
                                     std::size_t max_body_bytes)
    : accounts_(std::move(accounts)), max_body_bytes_(max_body_bytes)
{
}

HttpResponse AccountController::Handle(const HttpRequest& request) const
{
    if (request.body.size() > max_body_bytes_)
        return Error(413, "body_too_large", "Request body too large.");

    std::string_view method;
    if (request.path == "/api/v1/accounts" || request.path == "/api/v1/sessions" ||
        request.path == "/api/v1/sessions/renew") method = "POST";
    else if (request.path == "/api/v1/me") method = "GET";
    else if (request.path == "/api/v1/sessions/current") method = "DELETE";
    else return Error(404, "not_found", "API route not found.");

    if (request.method != method)
    {
        auto response = Error(405, "method_not_allowed", "Method not allowed.");
        response.headers["allow"] = method;
        return response;
    }

    try
    {
        if (request.path == "/api/v1/accounts" || request.path == "/api/v1/sessions")
        {
            if (!IsJson(request)) return Error(415, "unsupported_media_type", "JSON body required.");
            Json body;
            try { body = ParseObject(request.body); }
            catch (const Json::exception&) { return AccountFailure(AccountError::InvalidArgument); }
            catch (const std::invalid_argument&) { return AccountFailure(AccountError::InvalidArgument); }

            service::account::CreateAccountRequest input;
            if (!ReadString(body, "login", kMaxLoginBytes, input.login) ||
                !ReadString(body, "password", kMaxPasswordBytes, input.password))
                return AccountFailure(AccountError::InvalidArgument);

            if (request.path == "/api/v1/accounts")
            {
                if (!OnlyFields(body, {"login", "password", "display_name"}) ||
                    !ReadString(body, "display_name", 256, input.display_name, false))
                    return AccountFailure(AccountError::InvalidArgument);
                // TODO: define invitation/admin/self-registration policy first.
                // Merely enabling HTTP must never open public registration.
                return Error(501, "registration_not_implemented", "Account registration policy is not implemented.");
            }

            if (!OnlyFields(body, {"login", "password"}))
                return AccountFailure(AccountError::InvalidArgument);
            if (!accounts_) return AccountFailure(AccountError::NotConfigured);
            const auto result = accounts_->Login({std::move(input.login), std::move(input.password)});
            return result.Succeeded() ? SessionResponse(*result.value) : AccountFailure(result.error);
        }

        // These routes take only an opaque login-session bearer credential.
        // JWTs for a media room are handled separately by WebRtcService.
        if (!request.body.empty()) return AccountFailure(AccountError::InvalidArgument);
        const auto token = BearerToken(request);
        if (token.empty()) return AccountFailure(AccountError::Unauthenticated);
        if (!accounts_) return AccountFailure(AccountError::NotConfigured);

        if (request.path == "/api/v1/me")
        {
            const auto result = accounts_->ResolveSession(token);
            return result.Succeeded() ? JsonResponse(200, IdentityJson(*result.value)) : AccountFailure(result.error);
        }
        if (request.path == "/api/v1/sessions/renew")
        {
            const auto result = accounts_->RenewSession(token);
            return result.Succeeded() ? SessionResponse(*result.value) : AccountFailure(result.error);
        }

        const auto status = accounts_->Logout(token);
        if (!status.Succeeded()) return AccountFailure(status.error);
        return {204, {}, {{"cache-control", "no-store"}}};
    }
    catch (...)
    {
        // Keep implementation/storage details and credentials out of responses.
        return AccountFailure(AccountError::InternalError);
    }
}

}
