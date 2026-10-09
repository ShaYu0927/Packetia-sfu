#include "AppConfig.h"
#include "third/nlohmann/json.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace config {
namespace {
using Json = nlohmann::json;
void Keys(const Json& object, std::initializer_list<const char*> allowed) {
    if (!object.is_object()) throw std::invalid_argument("configuration section must be an object");
    for (auto it = object.begin(); it != object.end(); ++it)
        if (std::none_of(allowed.begin(), allowed.end(), [&](const char* key) { return it.key() == key; }))
            throw std::invalid_argument("unknown configuration key: " + it.key());
}
bool Flag(const Json& value) {
    if (!value.is_boolean()) throw std::invalid_argument("configuration flags must be JSON booleans");
    return value.get<bool>();
}
template<class T> void Number(const Json& object, const char* key, T& result, bool allow_zero = false) {
    if (!object.contains(key)) return;
    const auto& value = object.at(key);
    if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<int64_t>() < 0))
        throw std::invalid_argument(std::string(key) + " must be a nonnegative integer");
    const auto number = value.get<uint64_t>();
    if ((!allow_zero && !number) || number > std::numeric_limits<T>::max())
        throw std::invalid_argument(std::string(key) + " is out of range");
    result = static_cast<T>(number);
}
StreamOverrides Overrides(const Json& object) {
    StreamOverrides result;
    if (object.contains("recording") && !object.at("recording").is_null()) result.recording = Flag(object.at("recording"));
    if (object.contains("ai") && !object.at("ai").is_null()) result.ai = Flag(object.at("ai"));
    if (object.contains("conference_mix") && !object.at("conference_mix").is_null()) result.conference_mix = Flag(object.at("conference_mix"));
    return result;
}
bool EnvironmentFlag(const char* name, bool fallback) {
    const char* raw = std::getenv(name);
    if (!raw) return fallback;
    std::string value(raw);
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (value == "1" || value == "true" || value == "on" || value == "yes") return true;
    if (value == "0" || value == "false" || value == "off" || value == "no") return false;
    throw std::invalid_argument(std::string(name) + " expects 0/1, false/true, off/on or no/yes");
}
}
bool StreamFeatures::Enabled(Feature feature) const {
    switch (feature) {
    case Feature::Recording: return recording;
    case Feature::AI: return ai;
    case Feature::ConferenceMix: return conference_mix;
    }
    return false;
}
bool AppConfig::ServiceEnabled(Feature feature) const {
    return StreamFeatures{recording_enabled, ai_enabled, conference_mix_enabled}.Enabled(feature);
}
StreamFeatures AppConfig::Resolve(const StreamKey& key) const {
    auto result = stream_defaults;
    const auto it = streams.find(key);
    if (it != streams.end()) {
        result.recording = it->second.recording.value_or(result.recording);
        result.ai = it->second.ai.value_or(result.ai);
        result.conference_mix = it->second.conference_mix.value_or(result.conference_mix);
    }
    result.recording &= recording_enabled;
    result.ai &= ai_enabled;
    result.conference_mix &= conference_mix_enabled;
    return result;
}
void AppConfig::Validate() const {
    if (recording.segment_ms > static_cast<uint64_t>(INT64_MAX / 1000) || recording.reorder_ms > 5000)
        throw std::invalid_argument("invalid segment duration or reorder window (0..5000 ms)");
    if (listen_ip.empty() || !rtsp_port || !sip_port || !rtmp_port || !udp_port || !websocket_port || !io_threads)
        throw std::invalid_argument("invalid server address, port or thread count");
    if (!webrtc.max_sessions)
        throw std::invalid_argument("webrtc.max_sessions must be positive");
    if (!turn.max_sessions || !turn.max_allocations || !turn.max_allocations_per_user ||
        !turn.max_permissions || !turn.max_channels ||
        (turn.relay_port_min == 0 ? turn.relay_port_max != 0 : turn.relay_port_min > turn.relay_port_max))
        throw std::invalid_argument("invalid TURN port range or resource limits");
    if (turn.enabled && (turn.listen_ip.empty() || !turn.listen_port || turn.realm.empty()))
        throw std::invalid_argument("invalid TURN listener or realm");
    if (recording.directory.empty() || !recording.max_queue_frames || !recording.max_queue_bytes ||
        !recording.max_streams || !recording.max_pending_bytes || !recording.idle_timeout_ms ||
        !recording.worker_count || !recording.max_stream_queue_frames || !recording.max_stream_queue_bytes)
        throw std::invalid_argument("invalid recording directory or resource limits");
    for (const auto& entry : streams)
        if (entry.first.session_id.empty() || entry.first.stream_id.empty())
            throw std::invalid_argument("stream overrides require session_id and stream_id");
}
AppConfig AppConfig::FromJson(const std::string& text) {
    const auto root = Json::parse(text);
    Keys(root, {"server", "webrtc", "turn", "services", "recording", "stream_defaults", "streams"});
    AppConfig config;
    if (root.contains("server")) {
        const auto& server = root.at("server");
        Keys(server, {"listen_ip", "rtsp_port", "sip_port", "rtmp_port", "udp_port", "websocket_port", "io_threads"});
        if (server.contains("listen_ip")) config.listen_ip = server.at("listen_ip").get<std::string>();
        Number(server, "rtsp_port", config.rtsp_port);
        Number(server, "sip_port", config.sip_port);
        Number(server, "rtmp_port", config.rtmp_port);
        Number(server, "udp_port", config.udp_port);
        Number(server, "websocket_port", config.websocket_port);
        Number(server, "io_threads", config.io_threads);
    }
    if (root.contains("webrtc")) {
        const auto& rtc = root.at("webrtc");
        Keys(rtc, {"enabled", "public_ip", "token", "max_sessions"});
        if (rtc.contains("enabled")) config.webrtc.enabled = Flag(rtc.at("enabled"));
        if (rtc.contains("public_ip")) config.webrtc.public_ip = rtc.at("public_ip").get<std::string>();
        if (rtc.contains("token")) config.webrtc.token = rtc.at("token").get<std::string>();
        Number(rtc, "max_sessions", config.webrtc.max_sessions);
    }
    if (root.contains("services")) {
        const auto& services = root.at("services");
        Keys(services, {"recording", "ai", "conference_mix"});
        if (services.contains("recording")) config.recording_enabled = Flag(services.at("recording"));
        if (services.contains("ai")) config.ai_enabled = Flag(services.at("ai"));
        if (services.contains("conference_mix")) config.conference_mix_enabled = Flag(services.at("conference_mix"));
    }
    if (root.contains("turn")) {
        const auto& turn = root.at("turn");
        Keys(turn, {"enabled", "listen_ip", "listen_port", "dual_stack", "relay_bind_ip", "advertised_ip",
                    "relay_bind_ip_v6", "advertised_ip_v6", "realm", "relay_port_min", "relay_port_max",
                    "max_sessions", "max_allocations", "max_allocations_per_user", "max_permissions",
                    "max_channels", "local_test"});
        if (turn.contains("enabled")) config.turn.enabled = Flag(turn.at("enabled"));
        if (turn.contains("dual_stack")) config.turn.dual_stack = Flag(turn.at("dual_stack"));
        if (turn.contains("local_test")) config.turn.local_test = Flag(turn.at("local_test"));
#define READ_TURN_STRING(name) if (turn.contains(#name)) config.turn.name = turn.at(#name).get<std::string>()
        READ_TURN_STRING(listen_ip); READ_TURN_STRING(relay_bind_ip); READ_TURN_STRING(advertised_ip);
        READ_TURN_STRING(relay_bind_ip_v6); READ_TURN_STRING(advertised_ip_v6); READ_TURN_STRING(realm);
#undef READ_TURN_STRING
#define READ_TURN_LIMIT(name) Number(turn, #name, config.turn.name)
        READ_TURN_LIMIT(listen_port); READ_TURN_LIMIT(max_sessions); READ_TURN_LIMIT(max_allocations);
        READ_TURN_LIMIT(max_allocations_per_user); READ_TURN_LIMIT(max_permissions); READ_TURN_LIMIT(max_channels);
#undef READ_TURN_LIMIT
        Number(turn, "relay_port_min", config.turn.relay_port_min, true);
        Number(turn, "relay_port_max", config.turn.relay_port_max, true);
    }
    if (root.contains("stream_defaults")) {
        const auto& defaults = root.at("stream_defaults");
        Keys(defaults, {"recording", "ai", "conference_mix"});
        if (defaults.contains("recording")) config.stream_defaults.recording = Flag(defaults.at("recording"));
        if (defaults.contains("ai")) config.stream_defaults.ai = Flag(defaults.at("ai"));
        if (defaults.contains("conference_mix")) config.stream_defaults.conference_mix = Flag(defaults.at("conference_mix"));
    }
    if (root.contains("recording")) {
        const auto& recording = root.at("recording");
        Keys(recording, {"directory", "max_queue_frames", "max_queue_bytes", "max_streams", "max_pending_bytes",
                         "discovery_ms", "idle_timeout_ms", "segment_ms", "reorder_ms", "index_path", "worker_count",
                         "max_stream_queue_frames", "max_stream_queue_bytes"});
        if (recording.contains("directory")) config.recording.directory = recording.at("directory").get<std::string>();
        if (recording.contains("index_path")) config.recording.index_path = recording.at("index_path").get<std::string>();
#define READ_RECORD_LIMIT(name) Number(recording, #name, config.recording.name)
        READ_RECORD_LIMIT(max_queue_frames); READ_RECORD_LIMIT(max_queue_bytes); READ_RECORD_LIMIT(max_streams);
        READ_RECORD_LIMIT(max_pending_bytes); READ_RECORD_LIMIT(idle_timeout_ms); READ_RECORD_LIMIT(worker_count);
        READ_RECORD_LIMIT(max_stream_queue_frames); READ_RECORD_LIMIT(max_stream_queue_bytes);
#undef READ_RECORD_LIMIT
        Number(recording, "discovery_ms", config.recording.discovery_ms, true);
        Number(recording, "segment_ms", config.recording.segment_ms, true);
        Number(recording, "reorder_ms", config.recording.reorder_ms, true);
    }
    if (root.contains("streams")) {
        if (!root.at("streams").is_array()) throw std::invalid_argument("streams must be an array");
        for (const auto& stream : root.at("streams")) {
            Keys(stream, {"session_id", "stream_id", "recording", "ai", "conference_mix"});
            StreamKey key{stream.at("session_id").get<std::string>(), stream.at("stream_id").get<std::string>()};
            if (!config.streams.emplace(std::move(key), Overrides(stream)).second)
                throw std::invalid_argument("duplicate stream configuration");
        }
    }
    config.Validate();
    return config;
}
AppConfig AppConfig::FromFile(const std::string& path) {
    std::ifstream file(path);
    if (!file) throw std::invalid_argument("cannot open configuration file: " + path);
    std::ostringstream text;
    text << file.rdbuf();
    if (file.bad()) throw std::invalid_argument("cannot read configuration file: " + path);
    return FromJson(text.str());
}
AppConfig AppConfig::FromEnvironment() 
{
    AppConfig config;
    if (const char* path = std::getenv("PACKETIA_CONFIG")) config = FromFile(path);
    config.recording_enabled = EnvironmentFlag("PACKETIA_RECORDING", config.recording_enabled);
    config.ai_enabled = EnvironmentFlag("PACKETIA_AI", config.ai_enabled);
    config.conference_mix_enabled = EnvironmentFlag("PACKETIA_CONFERENCE_MIX", config.conference_mix_enabled);
    config.webrtc.enabled = EnvironmentFlag("PACKETIA_WEBRTC", config.webrtc.enabled);
    config.turn.enabled = EnvironmentFlag("PACKETIA_TURN", config.turn.enabled);
    if (const char* ip = std::getenv("PACKETIA_WEBRTC_PUBLIC_IP")) config.webrtc.public_ip = ip;
    if (const char* token = std::getenv("PACKETIA_WEBRTC_TOKEN")) config.webrtc.token = token;
    if (const char* directory = std::getenv("PACKETIA_RECORD_DIR")) config.recording.directory = directory;
    config.Validate();
    return config;
}
} // namespace config
