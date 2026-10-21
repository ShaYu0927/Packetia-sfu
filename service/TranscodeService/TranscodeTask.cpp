#include "TranscodeTask.h"

#include <utility>

namespace service::transcode {

TranscodeTask::TranscodeTask(TranscodeTaskConfig config) : config_(std::move(config)) {}

bool TranscodeTask::Validate(std::string& error) const
{
    error.clear();
    if (Id().empty() || !config_.input.Valid() || !config_.output.Valid() ||
        config_.input.endpoint_id == config_.output.endpoint_id)
        error = "task requires an ID, a source and a distinct valid output identity";
    else if (!config_.queue.Valid()) error = "task queue limits must be positive";
    else return config_.video.Validate(error);
    return false;
}

bool TranscodeTask::ConflictsWith(const TranscodeTask& other) const noexcept
{
    return Id() == other.Id() || config_.input.SameTrack(other.config_.input) ||
        config_.output.endpoint_id == other.config_.output.endpoint_id;
}

} // namespace service::transcode
