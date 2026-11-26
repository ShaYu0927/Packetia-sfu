#pragma once

#include <functional>
#include <string>

namespace service::ai
{
struct AgentRoomJoin
{
    std::string session_id;
    std::string room_id;
    std::string agent_id;
    std::string url;
    std::string token;
};

struct AgentRoomEvent
{
    enum class Kind { Joined, Closed, Failed };
    Kind kind;
    std::string participant_id;
    std::string error;
};

class AgentRoomClient
{
public:
    using Completion = std::function<void(AgentRoomEvent)>;
    virtual ~AgentRoomClient() = default;
    virtual bool Start() = 0;
    virtual void Stop() = 0;
    virtual bool Join(AgentRoomJoin request, Completion completion) = 0;
    virtual void Leave(const std::string& session_id) = 0;
};

}
