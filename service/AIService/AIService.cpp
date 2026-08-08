#include "AIService.h"
#include "logger.h"

#include <utility>

namespace service
{
namespace ai
{

AIService::AIService(std::shared_ptr<IModelProvider> provider,
                     std::shared_ptr<media::EncodedFrameRouter> frame_router,
                     std::shared_ptr<IAIFrameProcessor> frame_processor,
                     size_t frame_queue_size)
    : orchestrator_(std::move(provider)),
      frame_ingress_(std::make_shared<AIFrameIngress>(
          std::move(frame_router), std::move(frame_processor), frame_queue_size))
{
}

bool AIService::Init()
{
    ServiceState expected = ServiceState::Created;
    return state_.compare_exchange_strong(expected, ServiceState::Initialized) ||
           expected == ServiceState::Initialized;
}

bool AIService::Start()
{
    const ServiceState current = state_.load();
    if (current == ServiceState::Running)
    {
        return true;
    }
    if (current != ServiceState::Initialized && current != ServiceState::Stopped)
    {
        return false;
    }

    state_.store(ServiceState::Starting);
    if (!orchestrator_.Start() || !frame_ingress_ || !frame_ingress_->Start())
    {
        if (frame_ingress_)
        {
            frame_ingress_->Stop();
        }
        orchestrator_.Stop();
        error_code_.store(1);
        state_.store(ServiceState::Failed);
        return false;
    }

    error_code_.store(0);
    state_.store(ServiceState::Running);
    LOG_INFO("[AI_SERVICE] started, frame ingress ready");
    return true;
}

void AIService::Stop()
{
    const ServiceState current = state_.load();
    if (current != ServiceState::Running && current != ServiceState::Starting)
    {
        return;
    }
    state_.store(ServiceState::Stopping);
    if (frame_ingress_)
    {
        frame_ingress_->Stop();
    }
    orchestrator_.Stop();
    state_.store(ServiceState::Stopped);
    LOG_INFO("[AI_SERVICE] stopped");
}

void AIService::Shutdown()
{
    Stop();
    state_.store(ServiceState::Created);
    error_code_.store(0);
}

ServiceType AIService::Type() const
{
    return ServiceType::Ai;
}

ServiceState AIService::State() const
{
    return state_.load();
}

ServiceHealth AIService::Health() const
{
    ServiceHealth health;
    health.type = Type();
    health.state = State();
    health.error_code = error_code_.load();
    health.healthy = health.state == ServiceState::Running && orchestrator_.Healthy();
    return health;
}

AIRequestHandle AIService::Submit(AIRequest request,
                                  IModelProvider::ChunkCallback on_chunk,
                                  IModelProvider::CompleteCallback on_complete)
{
    return orchestrator_.Submit(std::move(request), std::move(on_chunk), std::move(on_complete));
}

bool AIService::Cancel(AIRequestHandle handle)
{
    return orchestrator_.Cancel(handle);
}

AIFrameIngressStats AIService::FrameStats() const
{
    return frame_ingress_ ? frame_ingress_->Stats() : AIFrameIngressStats{};
}

} // namespace ai
} // namespace service
