#pragma once

#include "service/RecordService/RecordingOptions.h"
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <tuple>

namespace config {
enum class Feature { Recording, AI, ConferenceMix };


struct StreamKey 
{
    std::string session_id;
    std::string stream_id;
    bool operator<(const StreamKey& other) const 
    {
        return std::tie(session_id, stream_id) < std::tie(other.session_id, other.stream_id);
    }
};

struct StreamFeatures 
{
    bool recording = true;
    bool ai = true;
    bool conference_mix = true;
    bool Enabled(Feature feature) const;
};

struct StreamOverrides 
{
    std::optional<bool> recording;
    std::optional<bool> ai;
    std::optional<bool> conference_mix;
};

struct WebRtcOptions 
{
    bool enabled = false;
    std::string public_ip;
    std::string token;
    std::size_t max_sessions = 128;
};

struct TurnOptions
{
    bool enabled = false;
    std::string listen_ip = "127.0.0.1";
    uint16_t listen_port = 3478;
    bool dual_stack = false;
    std::string relay_bind_ip = "127.0.0.1";
    std::string advertised_ip = "127.0.0.1";
    std::string relay_bind_ip_v6;
    std::string advertised_ip_v6;
    std::string realm = "packetia";
    uint16_t relay_port_min = 49152;
    uint16_t relay_port_max = 65535;
    std::size_t max_sessions = 512;
    std::size_t max_allocations = 128;
    std::size_t max_allocations_per_user = 8;
    std::size_t max_permissions = 64;
    std::size_t max_channels = 64;
    bool local_test = false;
};


struct AppConfig 
{
    std::string listen_ip = "0.0.0.0";
    uint16_t rtsp_port = 554, sip_port = 5060, rtmp_port = 1935;
    uint16_t udp_port = 9000, websocket_port = 8080;
    uint32_t io_threads = 1;
    WebRtcOptions webrtc;
    TurnOptions turn;
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
    static AppConfig FromEnvironment();
};
} // namespace config
