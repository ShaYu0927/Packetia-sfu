#pragma once

#include "ServiceBase.h"
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace service::meeting
{
class MeetingModule;
}

namespace service::ai
{
enum class AgentSessionState { Prepared, Stopped };

struct AgentSession
{
    std::string session_id;
    std::string room_id;
    std::string owner_id;
    AgentSessionState state = AgentSessionState::Prepared;
};


class VoiceAgentService final : public ServiceBase
{
public:
    explicit VoiceAgentService(std::shared_ptr<meeting::MeetingModule> meetings, std::size_t max_sessions = 128);
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
    static void ReleaseSession(AgentSession& session);

    const std::shared_ptr<meeting::MeetingModule> meetings_;
    const std::size_t max_sessions_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, AgentSession> sessions_;
    bool accepting_ = false;
};
}
