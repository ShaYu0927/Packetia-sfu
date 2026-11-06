#pragma once

#include "IService.h"
#include "TranscodeTaskTypes.h"

#include <functional>
#include <optional>

namespace service::transcode {

// Control/status boundary; workers and codec backends belong to the implementation.
// Lifecycle/task control must run outside codec and output-subscriber callbacks.
class ITranscodeService : public IService
{
public:
    virtual bool StartTask(TranscodeTaskConfig config, std::function<void()> request_keyframe = {}) = 0;
    virtual bool StopTask(const std::string& id, TranscodeTaskStatus* final_status = nullptr) = 0;
    virtual std::optional<TranscodeTaskStatus> TaskStatus(const std::string& id) const = 0;
    virtual std::string LastError() const = 0;
};

} // namespace service::transcode
