#pragma once

#include "service/RecordService/RecordingOptions.h"
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <tuple>

namespace config {
enum class Feature { Recording, AI, ConferenceMix };

// Audio/video tracks in one published stream share a policy.
struct StreamKey {
    std::string session_id;
    std::string stream_id;
    bool operator<(const StreamKey& other) const {
        return std::tie(session_id, stream_id) < std::tie(other.session_id, other.stream_id);
    }
};
struct StreamFeatures {
    bool recording = true;
    bool ai = true;
    bool conference_mix = true;
    bool Enabled(Feature feature) const;
};
struct StreamOverrides {
    std::optional<bool> recording;
    std::optional<bool> ai;
    std::optional<bool> conference_mix;
};
struct AppConfig {
    std::string listen_ip = "0.0.0.0";
    uint16_t rtsp_port = 554, sip_port = 5060, rtmp_port = 1935;
    uint16_t udp_port = 9000, websocket_port = 8080;
    uint32_t io_threads = 1;
    bool recording_enabled = true;
    bool ai_enabled = true;
    bool conference_mix_enabled = false;
    service::RecordingOptions recording;
    StreamFeatures stream_defaults;
    std::map<StreamKey, StreamOverrides> streams;

    bool ServiceEnabled(Feature feature) const;
    StreamFeatures Resolve(const StreamKey& stream) const;
    void Validate() const;
    static AppConfig FromJson(const std::string& text);
    static AppConfig FromFile(const std::string& path);
    // Built-in defaults < PACKETIA_CONFIG JSON file < environment overrides.
    static AppConfig FromEnvironment();
};
} // namespace config
