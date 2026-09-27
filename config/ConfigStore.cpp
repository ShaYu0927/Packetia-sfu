#include "ConfigStore.h"
#include <utility>

namespace config {
ConfigStore::ConfigStore(AppConfig config) {
    config.Validate();
    snapshot_ = std::make_shared<const AppConfig>(std::move(config));
}
std::shared_ptr<const AppConfig> ConfigStore::Snapshot() const { return std::atomic_load(&snapshot_); }
void ConfigStore::SetServiceEnabled(Feature feature, bool enabled) {
    Update([&](AppConfig& config) {
        switch (feature) {
        case Feature::Recording: config.recording_enabled = enabled; break;
        case Feature::AI: config.ai_enabled = enabled; break;
        case Feature::ConferenceMix: config.conference_mix_enabled = enabled; break;
        }
    });
}
void ConfigStore::SetStreamDefaults(StreamFeatures defaults) {
    Update([&](AppConfig& config) { config.stream_defaults = defaults; });
}
void ConfigStore::SetStreamConfig(StreamKey key, StreamOverrides overrides) {
    Update([&](AppConfig& config) { config.streams[std::move(key)] = overrides; });
}
void ConfigStore::RemoveStreamConfig(const StreamKey& key) {
    Update([&](AppConfig& config) { config.streams.erase(key); });
}
void ConfigStore::SetStreamRecording(const StreamKey& key, bool enabled) {
    Update([&](AppConfig& config) { config.streams[key].recording = enabled; });
}
} // namespace config
