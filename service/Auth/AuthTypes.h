#ifndef PACKETIA_SERVICE_AUTH_TYPES_H_
#define PACKETIA_SERVICE_AUTH_TYPES_H_

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

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


struct RoomGrant
{
    std::string room;             /* 权限适用的房间标识 */
    bool room_join = false;       /* 是否允许加入该房间 */
    bool can_publish = false;     /* 是否允许发布音视频 */
    bool can_subscribe = false;   /* 是否允许订阅他人的音视频 */
};


struct AuthContext
{
    std::string subject_id;       /* 身份标识 */
    std::string issuer;           /* 凭证签发方 */
    std::string name;              /* 可选的用户友好名称 */
    Audience audience = Audience::WebRtc; /* 凭证面向的服务，默认 WebRTC */
    std::string token_id;
    int64_t expires_at_unix_seconds = 0;  /* 到期时间：Unix 时间戳，单位秒*/
    RoomGrant video;                /* 音视频房间权限 */
    std::string node_id;
    std::string instance_id;
};

struct ResourceContext
{
    ResourceType type = ResourceType::Room; // 资源类型，默认房间
    std::string id;                         /* 目标资源自身的标识 */
    std::string room_id;          /* 资源所属房间的标识 */
    std::string owner_subject_id; /* 资源所有者的身份主体标识 */
    std::string session_id;       /* 关联的会话标识 */
    std::string stream_id;        /* 关联的媒体流标识 */
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
