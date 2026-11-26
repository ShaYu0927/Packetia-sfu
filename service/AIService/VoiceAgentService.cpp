#include "VoiceAgentService.h"
#include "MeetingModule.h"
#include "Coordinator.h"

namespace service::ai
{
using cluster::Failure;

VoiceAgentService::VoiceAgentService(std::shared_ptr<meeting::MeetingModule> meetings,
                                   std::size_t max_sessions, std::size_t max_pending)
    : VoiceAgentService(std::move(meetings), Options{true, max_sessions, max_pending, {}}) {}

VoiceAgentService::VoiceAgentService(std::shared_ptr<meeting::MeetingModule> meetings, Options options)
    : meetings_(std::move(meetings)), options_(options), executor_(options_.max_pending) {}

VoiceAgentService::~VoiceAgentService() { Shutdown(); }

bool VoiceAgentService::OnInit() { return meetings_ && options_.max_sessions > 0; }

bool VoiceAgentService::OnStart()
{
    if (meetings_->State() != ServiceState::Running) return false;
    if (options_.worker && !options_.worker->Start()) return false;
    try
    {
        if (executor_.Start([this] { ReleaseAllSessions(); })) return true;
    }
    catch (...)
    {
        if (options_.worker) options_.worker->Stop();
        throw;
    }
    if (options_.worker) options_.worker->Stop();
    return false;
}

void VoiceAgentService::OnStop()
{
    executor_.Stop();
    if (options_.worker) options_.worker->Stop();
}

void VoiceAgentService::ReleaseAllSessions()
{
    for (auto& entry : sessions_) ReleaseSession(entry.second);
    sessions_.clear();
}

void VoiceAgentService::OnShutdown() {} // OnStop 已完成清理。

void VoiceAgentService::RequireReady() const
{
    if (State() != ServiceState::Running)
        throw Failure(503, "agent_unavailable");
}

AgentSession VoiceAgentService::CreateSession(const std::string& room, const std::string& owner)
{
    RequireReady();
    return executor_.Submit([this, room, owner] { return CreateSessionOnAgent(room, owner); }).get();
}

AgentSession VoiceAgentService::CreateSessionOnAgent(const std::string& room, const std::string& owner)
{
    meetings_->RequireRoomOwner(room, owner);
    if (!options_.enabled) throw Failure(503, "agent_disabled");
    for (const auto& entry : sessions_)
        if (entry.second.room_id == room) throw Failure(409, "agent_already_exists");
    if (sessions_.size() >= options_.max_sessions) throw Failure(409, "agent_capacity_reached");

    AgentSession session;
    session.session_id = cluster::Coordinator::RandomId();
    session.room_id = room;
    session.owner_id = owner;
    AgentWorkerJob job;
    if (options_.worker)
    {
        session.agent_id = "agent-" + session.session_id;
        auto admission = meetings_->AdmitAgent(room, owner, session.agent_id);
        job = {session.session_id, room, session.agent_id,
            std::move(admission.assignment.node.signaling_url), std::move(admission.token)};
        session.state = AgentSessionState::Starting;
    }
    const auto inserted = sessions_.emplace(session.session_id, session);
    if (!inserted.second) throw Failure(503, "agent_id_conflict");
    if (options_.worker)
    {
        try
        {
            if (!options_.worker->StartSession(std::move(job), [this, id = session.session_id](AgentWorkerEvent event) {
                executor_.PostCompletion([this, id, event = std::move(event)]() mutable {
                    OnWorkerEvent(id, std::move(event));
                });
            })) throw Failure(503, "agent_unavailable");
        }
        catch (...)
        {
            sessions_.erase(inserted.first);
            throw;
        }
    }
    return session;
}

void VoiceAgentService::OnWorkerEvent(const std::string& session_id, AgentWorkerEvent event)
{
    const auto found = sessions_.find(session_id);
    if (found == sessions_.end()) return;
    auto& session = found->second;
    if (event.kind == AgentWorkerEvent::Kind::Stopped || session.state == AgentSessionState::Stopping)
    {
        if (event.kind != AgentWorkerEvent::Kind::Joined) sessions_.erase(found);
        return;
    }
    if (event.kind == AgentWorkerEvent::Kind::Joined)
    {
        session.participant_id = std::move(event.participant_id);
        session.state = AgentSessionState::Running;
        return;
    }
    session.error = std::move(event.error);
    session.state = AgentSessionState::Failed;
}

AgentSession VoiceAgentService::GetSession(const std::string& session, const std::string& owner) const
{
    RequireReady();
    return executor_.Submit([this, session, owner] {
        const auto found = sessions_.find(session);
        // 不向其他账号暴露该会话是否存在。
        if (found == sessions_.end() || found->second.owner_id != owner)
            throw Failure(404, "agent_not_found");
        return found->second;
    }).get();
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
    RequireReady();
    return executor_.Submit([this, session, owner] {
        auto& stored = RequireSession(session, owner);
        if (stored.state != AgentSessionState::Stopping) ReleaseSession(stored);
        const auto result = stored;
        if (result.state == AgentSessionState::Stopped) sessions_.erase(session);
        return result;
    }).get();
}

void VoiceAgentService::ReleaseSession(AgentSession& session)
{
    if (options_.worker && (session.state == AgentSessionState::Starting || session.state == AgentSessionState::Running))
    {
        options_.worker->StopSession(session.session_id);
        session.state = AgentSessionState::Stopping;
    }
    else session.state = AgentSessionState::Stopped;
}
}
