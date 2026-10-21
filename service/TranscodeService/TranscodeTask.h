#pragma once

#include "TranscodeTaskTypes.h"

namespace service::transcode {


class TranscodeTask final
{
public:
    explicit TranscodeTask(TranscodeTaskConfig config);
    const std::string& Id() const noexcept { return config_.task_id; }
    const TranscodeTaskConfig& Config() const noexcept { return config_; }
    bool Validate(std::string& error) const;
    bool ConflictsWith(const TranscodeTask& other) const noexcept;

private:
    const TranscodeTaskConfig config_;
};

} // namespace service::transcode
