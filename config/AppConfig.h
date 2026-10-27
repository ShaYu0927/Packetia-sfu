#pragma once

#include "RecordingOptions.h"
#include "AccountOptions.h"
#include "HttpServerOptions.h"
#include "TurnConstants.h"
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
    // JWT issuer (API key) -> HS256 secret; credentials remain server-side.
    std::map<std::string, std::string> api_keys;
    std::size_t max_sessions = 128;
    uint32_t reconnect_timeout_ms = 15000;
    uint32_t admission_timeout_ms = 10000;
    uint16_t signaling_ping_seconds = 30;
    uint16_t signaling_idle_timeout_seconds = 60;
};

struct TurnOptions
{
    bool enabled = false;
    std::string listen_ip = "127.0.0.1";
    uint16_t listen_port = PACKETIA_TURN_DEFAULT_LISTEN_PORT;
    bool dual_stack = false;
    std::string relay_bind_ip = "127.0.0.1";
    std::string advertised_ip = "127.0.0.1";
    std::string relay_bind_ip_v6;
    std::string advertised_ip_v6;
    std::string realm = "packetia";
    uint16_t relay_port_min = PACKETIA_TURN_DEFAULT_RELAY_PORT_MIN;
    uint16_t relay_port_max = PACKETIA_TURN_DEFAULT_RELAY_PORT_MAX;
    std::size_t max_sessions = PACKETIA_TURN_DEFAULT_MAX_SESSIONS;
    std::size_t max_allocations = PACKETIA_TURN_DEFAULT_MAX_ALLOCATIONS;
    std::size_t max_allocations_per_user = PACKETIA_TURN_DEFAULT_MAX_ALLOCATIONS_PER_USER;
    std::size_t max_permissions = PACKETIA_TURN_DEFAULT_MAX_PERMISSIONS;
    std::size_t max_channels = PACKETIA_TURN_DEFAULT_MAX_CHANNELS;
    bool local_test = false;
    bool unauthorized_ratelimit = false;
    uint32_t unauthorized_ratelimit_rps = PACKETIA_TURN_DEFAULT_UNAUTHORIZED_RPS;
};


struct WorkerOptions
{
    uint32_t media_threads = 4;
    uint32_t endpoint_threads = 4;
    // Zero selects min(max(hardware_concurrency / 2, 1), 4).
    uint32_t transcode_threads = 0;
    size_t media_queue_len = 4096;
    size_t endpoint_queue_len = 4096;
    size_t transcode_queue_len = 512;
};

struct AppConfig
{
    std::string listen_ip = "0.0.0.0";
    uint16_t rtsp_port = 554, sip_port = 5060, rtmp_port = 1935;
    uint16_t udp_port = 9000, websocket_port = 8080;
    uint32_t io_threads = 1;
    WorkerOptions workers;
    service::account::AccountOptions account;
    protocol::http::HttpServerOptions http;
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
    static AppConfig FromEnvironment(const std::string& config_path = {});
};
} // namespace config
