#include "TranscodeService.h"
#include "TranscodeTask.h"
#include "TranscodeSession.h"

#include <exception>
#include <utility>

namespace service::transcode {

TranscodeService::TranscodeService(std::shared_ptr<media::EncodedFrameRouter> input,
    std::shared_ptr<media::EncodedFrameRouter> output, size_t max_tasks, VideoTranscoderFactory backend_factory)
    : input_(std::move(input)), output_(std::move(output)), max_tasks_(max_tasks),
      backend_factory_(std::move(backend_factory)) {}

TranscodeService::~TranscodeService() { Stop(); }

bool TranscodeService::Init()
{
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    if (State() == ServiceState::Running) return true;
    if (!input_ || !output_ || input_ == output_ || !max_tasks_) {
        error_ = "distinct input/output routers and a positive task limit are required";
        state_ = ServiceState::Failed;
        return false;
    }
    state_ = ServiceState::Initialized;
    return true;
}

bool TranscodeService::Start()
{
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    if (State() == ServiceState::Running) return true;
    if (State() != ServiceState::Initialized && State() != ServiceState::Stopped) return false;
    if (!input_ || !output_ || input_ == output_ || !max_tasks_) return false;
    error_.clear();
    state_ = ServiceState::Running;
    return true;
}

void TranscodeService::Stop()
{
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
    state_ = ServiceState::Stopping;
    decltype(tasks_) tasks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks.swap(tasks_);
    }
    for (const auto& task : tasks) task.second.session->Stop();
    state_ = ServiceState::Stopped;
}

bool TranscodeService::StartTask(TranscodeTaskConfig config, KeyFrameRequest request_keyframe)
{
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
    const auto task = std::make_shared<const TranscodeTask>(std::move(config));
    const std::string id = task->Id();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (State() != ServiceState::Running) {
            error_ = "transcode service is not running";
            return false;
        }
        if (tasks_.count(id) || tasks_.size() >= max_tasks_) {
            error_ = "duplicate task ID or transcode task capacity exceeded";
            return false;
        }
        if (!task->Validate(error_)) return false;
        for (const auto& entry : tasks_) {
            if (task->ConflictsWith(*entry.second.task)) {
                error_ = "input track already has a task or output endpoint identity is already in use";
                return false;
            }
        }
    }
    std::shared_ptr<TranscodeSession> session;
    try {
        session = std::make_shared<TranscodeSession>(task, input_, output_, std::move(request_keyframe), backend_factory_);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tasks_.emplace(id, TaskRuntime{task, session});
        }
        if (session->Start()) {
            std::lock_guard<std::mutex> lock(mutex_);
            error_.clear();
            return true;
        }
        const auto status = session->Status();
        session->Stop();
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_.erase(id);
        error_ = status.last_error;
        return false;
    } catch (const std::exception& error) {
        if (session) session->Stop();
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_.erase(id);
        error_ = error.what();
        return false;
    }
}

bool TranscodeService::StopTask(const std::string& id, TranscodeTaskStatus* final_status)
{
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
    std::shared_ptr<TranscodeSession> session;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = tasks_.find(id);
        if (it == tasks_.end()) return false;
        session = it->second.session;
        tasks_.erase(it);
    }
    session->Stop();
    if (final_status) *final_status = session->Status();
    return true;
}

std::optional<TranscodeTaskStatus> TranscodeService::TaskStatus(const std::string& id) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = tasks_.find(id);
    return it == tasks_.end() ? std::nullopt : std::optional<TranscodeTaskStatus>(it->second.session->Status());
}

ServiceHealth TranscodeService::Health() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    bool healthy = State() == ServiceState::Running;
    for (const auto& task : tasks_) healthy &= task.second.session->Status().state == TranscodeTaskState::Running;
    return {healthy, Type(), State(), healthy ? 0 : 1};
}

std::string TranscodeService::LastError() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

} // namespace service::transcode
