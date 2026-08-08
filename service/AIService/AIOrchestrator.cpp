#include "AIOrchestrator.h"

#include <utility>

namespace service
{
namespace ai
{

AIOrchestrator::AIOrchestrator(std::shared_ptr<IModelProvider> provider)
    : provider_(std::move(provider))
{
}

bool AIOrchestrator::Start()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_)
    {
        return true;
    }
    started_ = provider_ && provider_->Start();
    return started_;
}

void AIOrchestrator::Stop()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (provider_ && started_)
    {
        provider_->Stop();
    }
    started_ = false;
}

bool AIOrchestrator::Healthy() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return started_ && provider_ && provider_->Healthy();
}

AIRequestHandle AIOrchestrator::Submit(AIRequest request,
                                       IModelProvider::ChunkCallback on_chunk,
                                       IModelProvider::CompleteCallback on_complete)
{
    std::shared_ptr<IModelProvider> provider;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (started_)
        {
            provider = provider_;
        }
    }

    if (!provider)
    {
        if (on_complete)
        {
            AIResponse response;
            response.request_id = request.request_id;
            response.error = "AI service is not running";
            on_complete(response);
        }
        return 0;
    }

    return provider->Submit(std::move(request), std::move(on_chunk), std::move(on_complete));
}

bool AIOrchestrator::Cancel(AIRequestHandle handle)
{
    std::shared_ptr<IModelProvider> provider;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        provider = started_ ? provider_ : nullptr;
    }
    return provider && provider->Cancel(handle);
}

} // namespace ai
} // namespace service
