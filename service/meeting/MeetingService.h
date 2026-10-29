#pragma once
#include "ClusterTypes.h"
#include <memory>
namespace service::cluster { class ClusterStore; }
namespace service::meeting
{
struct MemberGrant { bool can_publish = false; bool room_admin = false; };
// 长期会议记录和准入角色；在线参与者状态由 SFU 房间 owner 管理。
class MeetingService final
{
public:
    explicit MeetingService(std::shared_ptr<cluster::ClusterStore> store) : store_(std::move(store)) {}
    std::string CreateRoom(const std::string& owner);
    void SetMemberRole(const std::string& room, const std::string& owner, const std::string& user, bool can_publish);
    MemberGrant Grant(const std::string& room, const std::string& user);
    void RequireOwner(const std::string& room, const std::string& owner, bool allow_ended = false);
    // 先持久化 closing，禁止继续签发凭证；已结束的请求可安全重试。
    bool BeginEnd(const std::string& room, const std::string& owner);
private:
    std::shared_ptr<cluster::ClusterStore> store_;
};
}
