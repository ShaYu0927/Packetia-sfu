#include "AccountController.h"
#include "ApiResponse.h"
#include "service/Account/AccountService.h"
#include "third/nlohmann/json.hpp"

#include <algorithm>
#include <string_view>
#include <utility>
#include <functional>

namespace server::api
{
namespace
{

using Json = nlohmann::json;
using protocol::http::HttpRequest;
using protocol::http::HttpResponse;
using service::account::AccountError;
constexpr std::size_t kMaxLoginBytes = 256;
constexpr std::size_t kMaxPasswordBytes = 4096;
constexpr std::size_t kMaxSessionTokenBytes = 4096;

HttpResponse AccountFailure(AccountError error)
{
    switch (error)
    {
    case AccountError::InvalidArgument:
        return ErrorResponse(400, "invalid_request", "Invalid request.");
    case AccountError::Unauthenticated:
    case AccountError::InvalidCredentials:
    case AccountError::NotFound:
    case AccountError::UserDisabled:
    case AccountError::SessionExpired:
    case AccountError::SessionRevoked:
        // Do not reveal whether an account exists, is disabled or has sessions.
        return ErrorResponse(401, "unauthenticated", "Authentication required or credentials invalid.");
    case AccountError::AlreadyExists:
        return ErrorResponse(409, "already_exists", "Account cannot be created.");
    case AccountError::NotImplemented:
        return ErrorResponse(501, "not_implemented", "Account operation is not implemented.");
    case AccountError::NotConfigured:
    case AccountError::Unavailable:
        return ErrorResponse(503, "unavailable", "Account service unavailable.");
    case AccountError::None: // Success without a value is an internal error.
    case AccountError::StorageError:
    case AccountError::InternalError:
        return ErrorResponse(500, "internal_error", "Account operation failed.");
    }
    return ErrorResponse(500, "internal_error", "Account operation failed.");
}

char LowerAscii(char ch)
{
    return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch + ('a' - 'A')) : ch;
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


// This decorator owns its service dependency and performs the common login-
// session credential checks for every protected account route.
ApiRouter::Handler SessionRoute(std::shared_ptr<service::account::AccountService> accounts,
    std::function<HttpResponse(service::account::AccountService&, std::string_view)> handler)
{
    return [accounts = std::move(accounts), handler = std::move(handler)](const ApiRequest& request)
    {
        const auto token = BearerToken(request.http);
        if (token.empty()) return AccountFailure(AccountError::Unauthenticated);
        if (!accounts) return AccountFailure(AccountError::NotConfigured);
        return handler(*accounts, token);
    };
}

HttpResponse GetCurrentUser(service::account::AccountService& accounts, std::string_view token)
{
    const auto result = accounts.ResolveSession(token);
    return result.Succeeded() ? JsonResponse(200, IdentityJson(*result.value)) : AccountFailure(result.error);
}

HttpResponse RenewSession(service::account::AccountService& accounts, std::string_view token)
{
    const auto result = accounts.RenewSession(token);
    return result.Succeeded() ? SessionResponse(*result.value) : AccountFailure(result.error);
}

HttpResponse Logout(service::account::AccountService& accounts, std::string_view token)
{
    const auto status = accounts.Logout(token);
    if (!status.Succeeded()) return AccountFailure(status.error);
    return {204, {}, {{"cache-control", "no-store"}}};
}

}

AccountController::AccountController(std::shared_ptr<service::account::AccountService> accounts)
    : accounts_(std::move(accounts)) {}

void AccountController::RegisterRoutes(ApiRouter::Builder& router) const
{
    // Capture a value, never a raw this: the registered routes remain usable
    // after the registration module is destroyed.
    const auto controller = *this;
    router.PostJson("/api/v1/accounts", [controller](const ApiRequest& request) {
        return controller.RegisterAccount(request);
    });
    router.PostJson("/api/v1/sessions", [controller](const ApiRequest& request) {
        return controller.Login(request);
    });
    router.Get("/api/v1/me", SessionRoute(accounts_, GetCurrentUser));
    router.Post("/api/v1/sessions/renew", SessionRoute(accounts_, RenewSession), ApiRouter::BodyMode::Empty);
    router.Delete("/api/v1/sessions/current", SessionRoute(accounts_, Logout));
}

HttpResponse AccountController::RegisterAccount(const ApiRequest& request) const
{
    service::account::CreateAccountRequest input;
    if (!ReadString(request.json, "login", kMaxLoginBytes, input.login) ||
        !ReadString(request.json, "password", kMaxPasswordBytes, input.password) ||
        !OnlyFields(request.json, {"login", "password", "display_name"}) ||
        !ReadString(request.json, "display_name", 256, input.display_name, false))
        return AccountFailure(AccountError::InvalidArgument);
    // Registration remains unavailable until its admission policy is defined.
    return ErrorResponse(501, "registration_not_implemented", "Account registration policy is not implemented.");
}

HttpResponse AccountController::Login(const ApiRequest& request) const
{
    std::string login, password;
    if (!ReadString(request.json, "login", kMaxLoginBytes, login) ||
        !ReadString(request.json, "password", kMaxPasswordBytes, password) ||
        !OnlyFields(request.json, {"login", "password"}))
        return AccountFailure(AccountError::InvalidArgument);
    if (!accounts_) return AccountFailure(AccountError::NotConfigured);
    const auto result = accounts_->Login({std::move(login), std::move(password)});
    return result.Succeeded() ? SessionResponse(*result.value) : AccountFailure(result.error);
}

}
