#pragma once
#include "ServiceBase.h"
#include "RoomAdmissionService.h"
#include "RoomAdminService.h"
#include <condition_variable>
#include <mutex>

namespace service::meeting
{
struct MeetingModuleOptions
{
    std::string database_path;
    std::string jwt_issuer;
    std::string jwt_secret;
};

// 会议控制面的生命周期入口；业务对象只在一次受保护的操作中使用。
class MeetingModule final : public ServiceBase
{
public:
    MeetingModule(MeetingModuleOptions options, RoomCommandSender sender);
    ~MeetingModule() override;
    ServiceType Type() const override { return ServiceType::Meeting; }

    std::string CreateRoom(const std::string& owner);
    void SetMemberRole(const std::string& room, const std::string& owner, const std::string& user, bool can_publish);
    RoomAdmission Admit(const std::string& room, const std::string& user, const std::string& name);
    RoomCommandResult ManageRoom(const std::string& room, const std::string& owner, nlohmann::json command);
    RoomCommandResult EndRoom(const std::string& room, const std::string& owner);
    std::vector<cluster::RoomAssignment> RegisterNode(const cluster::Node& node);
    std::vector<cluster::RoomAssignment> HeartbeatNode(const std::string& node, const std::string& instance, int participants);
    bool ReleaseRoom(const std::string& node, const std::string& instance, const std::string& room, const std::string& assignment);

protected:
    bool OnInit() override;
    bool OnStart() override;
    void OnStop() override;
    void OnShutdown() override;

private:
    struct Runtime;
    struct Operation
    {
        MeetingModule& module;
        std::shared_ptr<Runtime> runtime;
        ~Operation();
        Operation(MeetingModule& module, std::shared_ptr<Runtime> runtime);
        Operation(const Operation&) = delete;
        Operation& operator=(const Operation&) = delete;
    };
    Operation Acquire();
    const MeetingModuleOptions options_;
    const RoomCommandSender sender_;
    std::mutex mutex_;
    std::condition_variable drained_;
    std::shared_ptr<Runtime> runtime_;
    std::size_t active_ = 0;
    bool accepting_ = false;
};
}
