#include "VoiceAgentService.h"
#include "MeetingModule.h"
#include "Coordinator.h"

namespace service::ai
{
using cluster::Failure;

VoiceAgentService::VoiceAgentService(std::shared_ptr<meeting::MeetingModule> meetings,
                                   std::size_t max_sessions)
    : meetings_(std::move(meetings)), max_sessions_(max_sessions) {}

VoiceAgentService::~VoiceAgentService() { Shutdown(); }

bool VoiceAgentService::OnInit() { return meetings_ && max_sessions_ > 0; }

bool VoiceAgentService::OnStart()
{
    std::lock_guard<std::mutex> lock(mutex_);
    accepting_ = meetings_->State() == ServiceState::Running;
    return accepting_;
}

void VoiceAgentService::OnStop()
{
    std::lock_guard<std::mutex> lock(mutex_);
    accepting_ = false;
    for (auto& entry : sessions_) ReleaseSession(entry.second);
    sessions_.clear();
}

void VoiceAgentService::OnShutdown() {} // OnStop 已完成清理。

void VoiceAgentService::RequireReady() const
{
    if (!accepting_ || State() != ServiceState::Running)
        throw Failure(503, "agent_unavailable");
}

AgentSession VoiceAgentService::CreateSession(const std::string& room, const std::string& owner)
{
    std::lock_guard<std::mutex> lock(mutex_);
    RequireReady();

    meetings_->RequireRoomOwner(room, owner);
    for (const auto& entry : sessions_)
        if (entry.second.room_id == room) throw Failure(409, "agent_already_exists");
    if (sessions_.size() >= max_sessions_) throw Failure(409, "agent_capacity_reached");

    AgentSession session{cluster::Coordinator::RandomId(), room, owner, AgentSessionState::Prepared};
    // TODO(练习 1)：在这里补充业务准备条件，例如检查 Agent 配置是否存在。
    // 当前只保存会话。后续音频订阅、ASR/LLM/TTS 应放到独立执行器，不能在此锁内调用网络接口。
    const auto inserted = sessions_.emplace(session.session_id, session);
    if (!inserted.second) throw Failure(503, "agent_id_conflict");
    return session;
}

AgentSession VoiceAgentService::GetSession(const std::string& session, const std::string& owner) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    RequireReady();
    const auto found = sessions_.find(session);
    // 不向其他账号暴露该会话是否存在。
    if (found == sessions_.end() || found->second.owner_id != owner)
        throw Failure(404, "agent_not_found");
    return found->second;
}

AgentSession& VoiceAgentService::RequireSession(const std::string& session, const std::string& owner)
{
    const auto found = sessions_.find(session);
    if (found == sessions_.end() || found->second.owner_id != owner)
        throw Failure(404, "agent_not_found");
    return found->second;
}

AgentSession VoiceAgentService::StopSession(const std::string& session, const std::string& owner)
{
    std::lock_guard<std::mutex> lock(mutex_);
    RequireReady();
    auto& stored = RequireSession(session, owner);
    ReleaseSession(stored);
    auto result = stored;
    sessions_.erase(session);
    return result;
}

void VoiceAgentService::ReleaseSession(AgentSession& session)
{
    // TODO(练习 2)：接入真实运行时后，先禁止新音频输入，再取消推理和播放，最后释放订阅。
    // 此函数也会在服务关闭时调用；当前没有媒体或推理资源需要释放。
    session.state = AgentSessionState::Stopped;
}
}
