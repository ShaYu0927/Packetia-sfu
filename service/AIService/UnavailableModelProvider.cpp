#include "UnavailableModelProvider.h"

#include <utility>

namespace service
{
namespace ai
{

bool UnavailableModelProvider::Start()
{
    started_.store(true);
    return true;
}

void UnavailableModelProvider::Stop()
{
    started_.store(false);
}

bool UnavailableModelProvider::Healthy() const
{
    return false;
}

std::string UnavailableModelProvider::Name() const
{
    return "unconfigured";
}

AIRequestHandle UnavailableModelProvider::Submit(AIRequest request,
                                                  ChunkCallback,
                                                  CompleteCallback on_complete)
{
    const AIRequestHandle handle = next_handle_.fetch_add(1);
    if (on_complete)
    {
        AIResponse response;
        response.request_id = std::move(request.request_id);
        response.error = "AI model provider is not configured";
        on_complete(response);
    }
    return handle;
}

bool UnavailableModelProvider::Cancel(AIRequestHandle)
{
    return false;
}

} // namespace ai
} // namespace service
