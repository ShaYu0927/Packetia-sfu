#include "ConferenceMixService.h"

#include <exception>
#include <utility>

namespace service::mix {
ConferenceMixService::ConferenceMixService(std::shared_ptr<media::EncodedFrameRouter> input,
                                         std::shared_ptr<media::EncodedFrameRouter> output,
                                         std::shared_ptr<const config::ConfigStore> policies)
    : input_(std::move(input)), output_(std::move(output)), policies_(std::move(policies)) {}

ConferenceMixService::~ConferenceMixService() { Stop(); }

bool ConferenceMixService::Init() {
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    if (State() == ServiceState::Running) return true;
    if (!input_ || !output_ || input_ == output_) {
        error_ = "input and output require distinct frame routers";
        state_ = ServiceState::Failed;
        return false;
    }
    state_ = ServiceState::Initialized;
    return true;
}

bool ConferenceMixService::Start() {
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    if (State() == ServiceState::Running) return true;
    if (State() != ServiceState::Initialized && State() != ServiceState::Stopped) return false;
    if (!input_ || !output_ || input_ == output_) return false;
    error_.clear();
    state_ = ServiceState::Running;
    return true;
}

void ConferenceMixService::Stop() {
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
    state_ = ServiceState::Stopping;
    decltype(tasks_) tasks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks.swap(tasks_);
    }
    // Do not hold the status mutex while joining workers: output callbacks
    // may query Health/LastError during the encoder flush.
    for (auto& entry : tasks) entry.second->Stop();
    state_ = ServiceState::Stopped;
}

bool ConferenceMixService::StartConference(MixConfig config, std::unique_ptr<IMixBackend> backend) {
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (State() != ServiceState::Running) {
            error_ = "conference mix service is disabled/not running";
            return false;
        }
        if (tasks_.count(config.room_id)) {
            error_ = "conference task already exists; stop it before starting another";
            return false;
        }
    }
    const auto room_id = config.room_id;
    std::shared_ptr<ConferenceMixer> task;
    try {
        task = std::make_shared<ConferenceMixer>(std::move(config), input_, output_, std::move(backend), policies_);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tasks_.emplace(room_id, task);
        }
        if (!task->Start()) {
            std::lock_guard<std::mutex> lock(mutex_);
            error_ = task->LastError();
            tasks_.erase(room_id);
            return false;
        }
    } catch (const std::exception& error) {
        if (task) task->Stop();
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_.erase(room_id);
        error_ = error.what();
        return false;
    } catch (...) {
        if (task) task->Stop();
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_.erase(room_id);
        error_ = "conference task startup failed";
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    error_.clear();
    return true;
}

bool ConferenceMixService::StopConference(const std::string& room_id) {
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
    std::shared_ptr<ConferenceMixer> task;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = tasks_.find(room_id);
        if (it == tasks_.end()) return false;
        task = std::move(it->second);
        tasks_.erase(it);
    }
    task->Stop();
    return true;
}

ServiceHealth ConferenceMixService::Health() const {
    std::lock_guard<std::mutex> lock(mutex_);
    bool healthy = State() == ServiceState::Running;
    for (const auto& entry : tasks_) healthy &= entry.second->State() == MixState::Running;
    return {healthy, Type(), State(), healthy ? 0 : 1};
}

std::string ConferenceMixService::LastError() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

void ConferenceMixService::RefreshStreamPolicies() {
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
    std::vector<std::shared_ptr<ConferenceMixer>> stopped;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = tasks_.begin(); it != tasks_.end();) {
            if (it->second->InputsEnabled()) { ++it; continue; }
            stopped.push_back(it->second);
            it = tasks_.erase(it);
        }
    }
    for (const auto& task : stopped) task->Stop();
}
} // namespace service::mix
