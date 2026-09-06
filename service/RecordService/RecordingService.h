#pragma once

#include "core/IService.h"
#include "RecordingOptions.h"
#include "core/EncodedFrameRouter.h"
#include <atomic>
#include <mutex>

namespace service {
class RecordingDispatcher;
struct RecordingStats {
    uint64_t accepted = 0, written = 0, dropped = 0, completed_files = 0, errors = 0;
    size_t queue_depth = 0, queue_bytes = 0;
};

class RecordingService final : public IService, public media::IEncodedFrameSink,
                               public std::enable_shared_from_this<RecordingService> {
public:
    explicit RecordingService(std::shared_ptr<media::EncodedFrameRouter> router,
                              RecordingOptions options = {});
    ~RecordingService() override;
    bool Init() override;
    bool Start() override;
    void Stop() override;
    void Shutdown() override { Stop(); }
    ServiceType Type() const override { return ServiceType::Record; }
    ServiceState State() const override { return state_.load(); }
    ServiceHealth Health() const override;
    RecordingStats Stats() const;
    bool TryEnqueue(const media::EncodedFrameEvent& event) override;
private:
    std::shared_ptr<media::EncodedFrameRouter> router_;
    RecordingOptions options_;
    std::mutex lifecycle_mutex_;
    mutable std::mutex mutex_;
    std::shared_ptr<RecordingDispatcher> dispatcher_;
    media::EncodedFrameRouter::SubscriptionId subscription_ = 0;
    std::atomic<ServiceState> state_{ServiceState::Created};
    std::atomic<uint64_t> accepted_{0}, written_{0}, dropped_{0}, completed_{0}, errors_{0};
};
}
