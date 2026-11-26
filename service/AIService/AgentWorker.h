#pragma once

#include <functional>
#include <string>

namespace service::ai
{
struct AgentWorkerJob
{
    std::string session_id;
    std::string room_id;
    std::string agent_id;
    std::string signaling_url;
    std::string join_token;
};

struct AgentWorkerEvent
{
    enum class Kind { Joined, Stopped, Failed };
    Kind kind;
    std::string participant_id;
    std::string error;
};

class AgentWorker
{
public:
    using EventHandler = std::function<void(AgentWorkerEvent)>;

    virtual ~AgentWorker() = default;
    virtual bool Start() = 0;
    virtual void Stop() = 0;
    virtual bool StartSession(AgentWorkerJob job, EventHandler on_event) = 0;
    virtual void StopSession(const std::string& session_id) = 0;
};

}
