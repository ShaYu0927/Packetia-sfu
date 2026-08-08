#ifndef _AI_ORCHESTRATOR_H_
#define _AI_ORCHESTRATOR_H_

#include "IModelProvider.h"

#include <memory>
#include <mutex>

namespace service
{
namespace ai
{

class AIOrchestrator
{
public:
    explicit AIOrchestrator(std::shared_ptr<IModelProvider> provider);

    bool Start();
    void Stop();
    bool Healthy() const;

    AIRequestHandle Submit(AIRequest request,
                           IModelProvider::ChunkCallback on_chunk,
                           IModelProvider::CompleteCallback on_complete);
    bool Cancel(AIRequestHandle handle);

private:
    mutable std::mutex mutex_;
    std::shared_ptr<IModelProvider> provider_;
    bool started_ = false;
};

} // namespace ai
} // namespace service

#endif /* _AI_ORCHESTRATOR_H_ */
