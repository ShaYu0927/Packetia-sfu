#ifndef _AI_SERVICE_H_
#define _AI_SERVICE_H_

#include "AIOrchestrator.h"
#include "AIFrameIngress.h"
#include "core/IService.h"

#include <atomic>
#include <memory>

namespace service
{
namespace ai
{

class AIService final : public IService
{
public:
    explicit AIService(std::shared_ptr<IModelProvider> provider,
                       std::shared_ptr<media::EncodedFrameRouter> frame_router,
                       std::shared_ptr<IAIFrameProcessor> frame_processor = nullptr,
                       size_t frame_queue_size = 128);

    bool Init() override;
    bool Start() override;
    void Stop() override;
    void Shutdown() override;

    ServiceType Type() const override;
    ServiceState State() const override;
    ServiceHealth Health() const override;

    AIRequestHandle Submit(AIRequest request,
                           IModelProvider::ChunkCallback on_chunk,
                           IModelProvider::CompleteCallback on_complete);
    bool Cancel(AIRequestHandle handle);
    AIFrameIngressStats FrameStats() const;

private:
    std::atomic<ServiceState> state_{ServiceState::Created};
    std::atomic<int> error_code_{0};
    AIOrchestrator orchestrator_;
    std::shared_ptr<AIFrameIngress> frame_ingress_;
};

} // namespace ai
} // namespace service

#endif /* _AI_SERVICE_H_ */
