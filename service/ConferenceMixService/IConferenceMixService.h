#pragma once

#include "IService.h"
#include "IMixBackend.h"

#include <memory>

namespace service::mix {

// Task control is serialized by the application, outside backend/output callbacks.
class IConferenceMixService : public IService
{
public:
    virtual bool StartConference(MixConfig config, std::unique_ptr<IMixBackend> backend) = 0;
    virtual bool StopConference(const std::string& room_id) = 0;
    virtual void RefreshStreamPolicies() = 0;
};

} // namespace service::mix
