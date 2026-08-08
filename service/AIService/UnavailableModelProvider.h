#ifndef _UNAVAILABLE_MODEL_PROVIDER_H_
#define _UNAVAILABLE_MODEL_PROVIDER_H_

#include "IModelProvider.h"

#include <atomic>

namespace service
{
namespace ai
{

// Safe default used until a real HTTP/local-model provider is configured.
class UnavailableModelProvider final : public IModelProvider
{
public:
    bool Start() override;
    void Stop() override;
    bool Healthy() const override;
    std::string Name() const override;

    AIRequestHandle Submit(AIRequest request,
                           ChunkCallback on_chunk,
                           CompleteCallback on_complete) override;
    bool Cancel(AIRequestHandle handle) override;

private:
    std::atomic<bool> started_{false};
    std::atomic<AIRequestHandle> next_handle_{1};
};

} // namespace ai
} // namespace service

#endif /* _UNAVAILABLE_MODEL_PROVIDER_H_ */
