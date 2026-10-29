#include "MeetingModule.h"
#include "ClusterStore.h"

namespace service::meeting
{
struct MeetingModule::Runtime
{
    std::shared_ptr<cluster::ClusterStore> store;
    std::shared_ptr<cluster::Coordinator> nodes;
    std::shared_ptr<MeetingService> meetings;
    std::shared_ptr<RoomAdmissionService> admissions;
    std::shared_ptr<RoomAdminService> admin;
};

MeetingModule::MeetingModule(MeetingModuleOptions options, RoomCommandSender sender)
    : options_(std::move(options)), sender_(std::move(sender)) {}
MeetingModule::~MeetingModule() { Shutdown(); }

bool MeetingModule::OnInit()
{
    if (options_.database_path.empty() || !cluster::Coordinator::ValidId(options_.jwt_issuer) ||
        options_.jwt_secret.size() < 32 || options_.jwt_secret.size() > 4096 || !sender_)
    { SetFailed(1); return false; }
    // 局部构造，数据库或对象初始化失败时自动释放本轮资源。
    auto runtime = std::make_shared<Runtime>();
    runtime->store = std::make_shared<cluster::ClusterStore>(options_.database_path);
    runtime->nodes = std::make_shared<cluster::Coordinator>(runtime->store);
    runtime->meetings = std::make_shared<MeetingService>(runtime->store);
    runtime->admissions = std::make_shared<RoomAdmissionService>(runtime->meetings, runtime->nodes,
        options_.jwt_issuer, options_.jwt_secret);
    runtime->admin = std::make_shared<RoomAdminService>(runtime->meetings, runtime->nodes, sender_);
    std::lock_guard<std::mutex> lock(mutex_);
    runtime_ = std::move(runtime);
    return true;
}
bool MeetingModule::OnStart()
{
    std::lock_guard<std::mutex> lock(mutex_);
    accepting_ = runtime_ != nullptr;
    return accepting_;
}
void MeetingModule::OnStop()
{
    std::unique_lock<std::mutex> lock(mutex_);
    accepting_ = false;
    // 业务操作不持有此锁执行数据库或网络 I/O；等待会释放锁。
    drained_.wait(lock, [this] { return active_ == 0; });
}
void MeetingModule::OnShutdown()
{
    std::lock_guard<std::mutex> lock(mutex_);
    accepting_ = false;
    runtime_.reset();
}
MeetingModule::Operation::Operation(MeetingModule& owner, std::shared_ptr<Runtime> value)
    : module(owner), runtime(std::move(value)) {}
MeetingModule::Operation::~Operation()
{
    // 先归还资源引用，再宣布操作结束，Shutdown 才能完整关闭数据库。
    runtime.reset();
    std::lock_guard<std::mutex> lock(module.mutex_);
    if (--module.active_ == 0) module.drained_.notify_all();
}
MeetingModule::Operation MeetingModule::Acquire()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!accepting_ || State() != ServiceState::Running || !runtime_)
        throw cluster::Failure(503, "meeting_unavailable");
    ++active_;
    return Operation(*this, runtime_);
}
std::string MeetingModule::CreateRoom(const std::string& owner)
{
    const auto operation = Acquire();
    return operation.runtime->meetings->CreateRoom(owner);
}
void MeetingModule::SetMemberRole(const std::string& room, const std::string& owner, const std::string& user, bool publish)
{
    const auto operation = Acquire();
    operation.runtime->meetings->SetMemberRole(room, owner, user, publish);
}
RoomAdmission MeetingModule::Admit(const std::string& room, const std::string& user, const std::string& name)
{
    const auto operation = Acquire();
    return operation.runtime->admissions->Admit(room, user, name);
}
RoomCommandResult MeetingModule::ManageRoom(const std::string& room, const std::string& owner, nlohmann::json command)
{
    const auto operation = Acquire();
    return operation.runtime->admin->Execute(room, owner, std::move(command));
}
RoomCommandResult MeetingModule::EndRoom(const std::string& room, const std::string& owner)
{
    const auto operation = Acquire();
    return operation.runtime->admin->EndRoom(room, owner);
}
std::vector<cluster::RoomAssignment> MeetingModule::RegisterNode(const cluster::Node& node)
{
    const auto operation = Acquire();
    operation.runtime->nodes->Register(node);
    return operation.runtime->nodes->ReclaimCandidates(node.id, node.instance);
}
std::vector<cluster::RoomAssignment> MeetingModule::HeartbeatNode(const std::string& node, const std::string& instance, int participants)
{
    const auto operation = Acquire();
    operation.runtime->nodes->Heartbeat(node, instance, participants);
    return operation.runtime->nodes->ReclaimCandidates(node, instance);
}
bool MeetingModule::ReleaseRoom(const std::string& node, const std::string& instance, const std::string& room, const std::string& assignment)
{
    const auto operation = Acquire();
    return operation.runtime->nodes->ReleaseRoom(node, instance, room, assignment);
}
}
