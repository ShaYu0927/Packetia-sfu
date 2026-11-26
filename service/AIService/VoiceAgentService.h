#pragma once

#include "ServiceBase.h"
#include "AgentExecutor.h"
#include "AgentWorker.h"
#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>

namespace service::meeting
{
class MeetingModule;
}

namespace service::ai
{

enum class AgentSessionState
{
    Prepared,  // 本地记录已创建
    Starting,  // 正在分发、等待入房
    Running,   // Worker 已报告就绪
    Stopping,
    Stopped,
    Failed
};

struct AgentSession
{
    std::string session_id;
    std::string room_id;
    std::string owner_id;

    std::string agent_id;        // 机器人业务身份
    std::string participant_id;  // SFU 入房后返回
    std::string error;

    AgentSessionState state = AgentSessionState::Prepared;
};


class VoiceAgentService final : public ServiceBase
{
public:
    struct Options
    {
        bool enabled = true;
        std::size_t max_sessions = 128;
        std::size_t max_pending = 128;
        std::shared_ptr<AgentWorker> worker;
    };

    explicit VoiceAgentService(std::shared_ptr<meeting::MeetingModule> meetings,
                               std::size_t max_sessions = 128, std::size_t max_pending = 128);
    VoiceAgentService(std::shared_ptr<meeting::MeetingModule> meetings, Options options);
    ~VoiceAgentService() override;
    ServiceType Type() const override { return ServiceType::VoiceAgent; }

    AgentSession CreateSession(const std::string& room, const std::string& owner);
    AgentSession GetSession(const std::string& session, const std::string& owner) const;
    AgentSession StopSession(const std::string& session, const std::string& owner);

protected:
    bool OnInit() override;
    bool OnStart() override;
    void OnStop() override;
    void OnShutdown() override;

private:
    void RequireReady() const;
    AgentSession& RequireSession(const std::string& session, const std::string& owner);
    AgentSession CreateSessionOnAgent(const std::string& room, const std::string& owner);
    void ReleaseAllSessions();
    void ReleaseSession(AgentSession& session);
    void OnWorkerEvent(const std::string& session_id, AgentWorkerEvent event);

    const std::shared_ptr<meeting::MeetingModule> meetings_;
    const Options options_;
    // 会话表只允许 Agent 线程访问；公共接口通过 executor_ 投递并返回快照。
    std::unordered_map<std::string, AgentSession> sessions_;
    AgentExecutor executor_;
};
}
