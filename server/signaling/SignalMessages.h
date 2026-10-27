#pragma once
#include "third/nlohmann/json.hpp"
#include "service/Auth/AuthTypes.h"
#include <cstdint>
#include <string>
#include <string_view>

namespace server::signaling
{
using Json = nlohmann::json;
inline constexpr std::size_t kMaxSdpBytes = 64 * 1024;
inline constexpr std::size_t kMaxMessageBytes = 6 * kMaxSdpBytes + 4096;
struct SignalKey
{
    std::string connection;
    uint64_t generation = 0;
    bool operator==(const SignalKey& other) const
    { return connection == other.connection && generation == other.generation; }
};
// 沿用现有 type 协议；消息拥有 JSON 数据，跨异步任务不借用接收缓冲。
struct SignalRequest
{
    enum class Type { Unknown, Leave, Join, Resume, Tracks, Publish, Subscribe, Offer, Answer, Rollback, RestartIce };

    static Type ParseType(std::string_view name)
    {
        static constexpr std::pair<std::string_view, Type> types[] = {
            {"close", Type::Leave}, {"leave", Type::Leave},
            {"join", Type::Join}, {"resume", Type::Resume}, {"tracks", Type::Tracks},
            {"publish", Type::Publish}, {"subscribe", Type::Subscribe},
            {"offer", Type::Offer}, {"answer", Type::Answer}, {"rollback", Type::Rollback},
            {"restart_ice", Type::RestartIce}
        };
        for (const auto& type : types)
            if (type.first == name) return type.second;
        return Type::Unknown;
    }

    Type type = Type::Unknown;
    Json body;
    // 边界解析错误也进入同一个请求 FIFO，避免越过先前请求的响应。
    Json rejection;
};

struct SignalResponse
{
    Json body;
    bool close_after = false;
    bool initial = false;
    uint64_t admission_generation = 0;
};
inline Json Error(const std::string& error)
{ return Json{{"type", "error"}, {"error", error}}; }
inline Json AuthError(service::auth::AuthError error)
{
    const char* code = "unauthenticated";
    if (error == service::auth::AuthError::Forbidden) code = "forbidden";
    else if (error == service::auth::AuthError::ExpiredToken) code = "token_expired";
    return Json{{"type", "error"}, {"error", error == service::auth::AuthError::Forbidden ? "Forbidden" : "Unauthorized"}, {"code", code}};
}
inline service::auth::ResourceContext RoomResource(const std::string& room)
{
    service::auth::ResourceContext resource;
    resource.type = service::auth::ResourceType::Room;
    resource.id = resource.room_id = room;
    return resource;
}
// 只负责现有 JSON 协议的边界检查，不创建房间或解释 SDP。
bool DecodeRequest(const std::string& text, SignalRequest& request, Json& error);
}
