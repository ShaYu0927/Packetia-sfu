#ifndef _I_MODEL_PROVIDER_H_
#define _I_MODEL_PROVIDER_H_

#include "AITypes.h"

#include <functional>
#include <string>

namespace service
{
namespace ai
{

class IModelProvider
{
public:
    using ChunkCallback = std::function<void(const AIChunk&)>;
    using CompleteCallback = std::function<void(const AIResponse&)>;

    virtual ~IModelProvider() = default;

    virtual bool Start() = 0;
    virtual void Stop() = 0;
    virtual bool Healthy() const = 0;
    virtual std::string Name() const = 0;

    virtual AIRequestHandle Submit(AIRequest request,
                                   ChunkCallback on_chunk,
                                   CompleteCallback on_complete) = 0;
    virtual bool Cancel(AIRequestHandle handle) = 0;
};

} // namespace ai
} // namespace service

#endif /* _I_MODEL_PROVIDER_H_ */
