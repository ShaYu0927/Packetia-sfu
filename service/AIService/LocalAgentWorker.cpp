#include "LocalAgentWorker.h"
#include "AgentRoomClient.h"
#include <stdexcept>
#include <utility>

namespace service::ai
{
namespace
{
class LocalAgentWorker final : public AgentWorker
{
public:
    explicit LocalAgentWorker(std::shared_ptr<AgentRoomClient> room_client)
        : room_client_(std::move(room_client)) {}

    bool Start() override { return room_client_->Start(); }
    void Stop() override { room_client_->Stop(); }

    bool StartSession(AgentWorkerJob job, EventHandler on_event) override
    {
        return room_client_->Join({std::move(job.session_id), std::move(job.room_id), std::move(job.agent_id),
            std::move(job.signaling_url), std::move(job.join_token)}, [on_event = std::move(on_event)](AgentRoomEvent event) {
            switch (event.kind)
            {
            case AgentRoomEvent::Kind::Joined:
                on_event({AgentWorkerEvent::Kind::Joined, std::move(event.participant_id), {}});
                break;
            case AgentRoomEvent::Kind::Closed:
                on_event({AgentWorkerEvent::Kind::Stopped, {}, {}});
                break;
            case AgentRoomEvent::Kind::Failed:
                on_event({AgentWorkerEvent::Kind::Failed, {}, std::move(event.error)});
                break;
            }
        });
    }

    void StopSession(const std::string& session_id) override { room_client_->Leave(session_id); }

private:
    const std::shared_ptr<AgentRoomClient> room_client_;
};
}

std::shared_ptr<AgentWorker> MakeLocalAgentWorker(std::shared_ptr<AgentRoomClient> room_client)
{
    if (!room_client) throw std::invalid_argument("Agent worker requires a room client");
    return std::make_shared<LocalAgentWorker>(std::move(room_client));
}
}
