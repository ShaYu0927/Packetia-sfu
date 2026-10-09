#ifndef PACKETIA_SERVICE_AUTH_TYPES_H_
#define PACKETIA_SERVICE_AUTH_TYPES_H_

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace service::auth
{

enum class Audience { WebRtc, Control };

enum class Action
{
    JoinRoom,
    ListTracks,
    Publish,
    Subscribe,
    ControlRecording,
    QueryRecording,
    ControlConference,
    ConfigureStream,
    ManageService,
    ReadConfig
};

enum class ResourceType { Room, Track, Stream, Server };

struct PermissionGrant
{
    ResourceType resource_type = ResourceType::Room;
    std::string resource_id;
    std::vector<Action> actions;
};


struct AuthContext
{
    std::string subject_id;
    Audience audience = Audience::WebRtc;
    std::string token_id;
    int64_t expires_at_unix_seconds = 0;
    std::vector<PermissionGrant> grants;
};

struct ResourceContext
{
    ResourceType type = ResourceType::Room;
    std::string id;
    std::string room_id;
    std::string owner_subject_id;
    std::string session_id;
    std::string stream_id;
};

enum class AuthError
{
    None,
    Unauthenticated,
    InvalidToken,
    ExpiredToken,
    WrongAudience,
    Forbidden,
    NotConfigured,
    InternalError
};

struct AuthResult
{
    AuthError error = AuthError::Unauthenticated;
    std::optional<AuthContext> context;

    bool Succeeded() const { return error == AuthError::None && context.has_value(); }

    static AuthResult Success(AuthContext context)
    {
        return {AuthError::None, std::move(context)};
    }

    static AuthResult Failure(AuthError error)
    {
        return {error == AuthError::None ? AuthError::InternalError : error, std::nullopt};
    }
};

struct AuthDecision
{
    AuthError error = AuthError::Forbidden;

    bool Allowed() const { return error == AuthError::None; }

    static AuthDecision Allow() { return {AuthError::None}; }

    static AuthDecision Deny(AuthError error = AuthError::Forbidden)
    {
        return {error == AuthError::None ? AuthError::Forbidden : error};
    }
};

}

#endif // PACKETIA_SERVICE_AUTH_TYPES_H_
