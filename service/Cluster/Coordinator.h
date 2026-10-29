#pragma once
#include "ClusterTypes.h"
#include <functional>
#include <memory>
#include <optional>
#include <vector>
namespace service::cluster
{
class ClusterStore;
// 只管理节点租约、实例隔离和房间路由；不负责房主、成员角色或 JWT。
class Coordinator final
{
public:
    using Clock = std::function<int64_t()>;
    static constexpr int kLeaseSeconds = 10;
    static constexpr int kAdmissionHoldSeconds = 90;
    explicit Coordinator(std::shared_ptr<ClusterStore> store, Clock clock = {});
    explicit Coordinator(const std::string& path, Clock clock = {});
    ~Coordinator();
    void Register(const Node& node);
    void Heartbeat(const std::string& node, const std::string& instance, int participants);
    RoomAssignment AssignRoom(const std::string& room);
    std::optional<RoomAssignment> FindRoom(const std::string& room, bool closing = false);
    std::vector<RoomAssignment> ReclaimCandidates(const std::string& node, const std::string& instance);
    bool ReleaseRoom(const std::string& node, const std::string& instance,
                     const std::string& room, const std::string& assignment);
    static int64_t UnixSeconds();
    static std::string RandomId();
    static bool ValidId(const std::string& value);
    static bool ValidSignalingUrl(const std::string& value);
    static bool ValidManagementUrl(const std::string& value);
private:
    std::shared_ptr<ClusterStore> store_;
    Clock clock_;
};
}
