#pragma once

#include "ConferenceMixer.h"
#include "service/core/IService.h"

#include <atomic>
#include <unordered_map>

namespace service::mix {

// Global feature gate and owner of conference tasks. Lifecycle/task control
// must not be called from a backend or output-subscriber callback.
class ConferenceMixService final : public IService {
public:
    ConferenceMixService(std::shared_ptr<media::EncodedFrameRouter> input,
                         std::shared_ptr<media::EncodedFrameRouter> output,
                         std::shared_ptr<const config::ConfigStore> policies = {});
    ~ConferenceMixService() override;
    bool Init() override;
    bool Start() override;
    void Stop() override;
    void Shutdown() override { Stop(); }
    ServiceType Type() const override { return ServiceType::ConferenceMix; }
    ServiceState State() const override { return state_.load(); }
    ServiceHealth Health() const override;

    // Enabling the service permits new tasks; it does not create a backend.
    bool StartConference(MixConfig config, std::unique_ptr<IMixBackend> backend);
    bool StopConference(const std::string& room_id);
    std::string LastError() const;
    // Stop only tasks containing inputs no longer permitted by stream policy.
    void RefreshStreamPolicies();

private:
    std::shared_ptr<media::EncodedFrameRouter> input_, output_;
    std::shared_ptr<const config::ConfigStore> policies_;
    std::mutex lifecycle_mutex_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<ConferenceMixer>> tasks_;
    std::atomic<ServiceState> state_{ServiceState::Created};
    std::string error_;
};
} // namespace service::mix
