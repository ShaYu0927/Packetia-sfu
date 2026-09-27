#pragma once

#include "AppConfig.h"
#include <memory>
#include <mutex>

namespace config {
// App-owned configuration, not a process singleton. Only the control layer
// mutates settings and reconciles lifecycle; media callbacks read snapshots.
class ConfigStore {
public:
    explicit ConfigStore(AppConfig config = {});
    std::shared_ptr<const AppConfig> Snapshot() const;
    StreamFeatures Resolve(const StreamKey& stream) const { return Snapshot()->Resolve(stream); }
    bool Allows(Feature feature, const StreamKey& stream) const { return Resolve(stream).Enabled(feature); }
    void SetServiceEnabled(Feature feature, bool enabled);
    void SetStreamDefaults(StreamFeatures defaults);
    void SetStreamConfig(StreamKey key, StreamOverrides overrides);
    // Recording commands change only this field, retaining AI/mix overrides.
    void SetStreamRecording(const StreamKey& key, bool enabled);
    void RemoveStreamConfig(const StreamKey& key);
private:
    template<class Change> void Update(Change change) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto next = std::make_shared<AppConfig>(*Snapshot());
        change(*next);
        next->Validate();
        std::atomic_store(&snapshot_, std::shared_ptr<const AppConfig>(std::move(next)));
    }
    mutable std::mutex mutex_;
    std::shared_ptr<const AppConfig> snapshot_;
};
} // namespace config
