#pragma once

#include "IService.h"
#include "TranscodeTaskTypes.h"
#include "TranscodeRuntime.h"

#include <atomic>
#include <optional>
#include <mutex>
#include <unordered_map>

namespace service::transcode {

class TranscodeService final : public IService
{
public:
    TranscodeService(std::shared_ptr<media::EncodedFrameRouter> input,
                     std::shared_ptr<media::EncodedFrameRouter> output, size_t max_tasks = 4,
                     VideoTranscoderFactory backend_factory = {});
    ~TranscodeService() override;
    bool Init() override;
    bool Start() override;
    void Stop() override;
    void Shutdown() override { Stop(); }
    ServiceType Type() const override { return ServiceType::Transcode; }
    ServiceState State() const override { return state_.load(); }
    ServiceHealth Health() const override;

    bool StartTask(TranscodeTaskConfig config, KeyFrameRequest request_keyframe = {});
    bool StopTask(const std::string& id, TranscodeTaskStatus* final_status = nullptr);
    std::optional<TranscodeTaskStatus> TaskStatus(const std::string& id) const;
    std::string LastError() const;

private:
    struct TaskRuntime {
        std::shared_ptr<const TranscodeTask> task;
        std::shared_ptr<TranscodeSession> session;
    };
    const std::shared_ptr<media::EncodedFrameRouter> input_, output_;
    const size_t max_tasks_;
    const VideoTranscoderFactory backend_factory_;
    std::mutex lifecycle_mutex_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, TaskRuntime> tasks_;
    std::atomic<ServiceState> state_{ServiceState::Created};
    std::string error_;
};

} // namespace service::transcode
