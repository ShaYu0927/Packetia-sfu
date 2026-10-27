#include "ServerConfig.h"
#include "ConfigStore.h"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <atomic>
#include <thread>
#include <filesystem>
#include <fstream>

namespace {
void Set(const char* name, const char* value) {
#ifdef _WIN32
    if (_putenv_s(name, value ? value : "") != 0) throw std::runtime_error("environment update failed");
#else
    if ((value ? setenv(name, value, 1) : unsetenv(name)) != 0)
        throw std::runtime_error("environment update failed");
#endif
}
struct SavedEnvironment {
    const char* name;
    std::optional<std::string> original;
    explicit SavedEnvironment(const char* key) : name(key) {
        if (const char* value = std::getenv(name)) original = value;
        Set(name, nullptr);
    }
    ~SavedEnvironment() { Set(name, original ? original->c_str() : nullptr); }
};
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void WorkerConfiguration() {
    const auto configured = config::AppConfig::FromJson(R"({
        "server":{"io_threads":3},
        "workers":{"media_threads":2,"endpoint_threads":1,"transcode_threads":3,
                   "media_queue_len":128,"endpoint_queue_len":64,"transcode_queue_len":16}
    })");
    Check(configured.io_threads == 3 && configured.workers.media_threads == 2 &&
          configured.workers.endpoint_threads == 1 && configured.workers.transcode_threads == 3 &&
          configured.workers.media_queue_len == 128 && configured.workers.endpoint_queue_len == 64 &&
          configured.workers.transcode_queue_len == 16, "worker configuration ignored");
    Check(config::AppConfig::FromJson(R"({"workers":{"transcode_threads":0}})").workers.transcode_threads == 0,
          "automatic transcode thread count must remain supported");
    for (const char* invalid : {
        R"({"server":{"io_threads":65}})", R"({"workers":{"media_threads":0}})",
        R"({"workers":{"endpoint_threads":65}})", R"({"workers":{"transcode_threads":-1}})",
        R"({"workers":{"transcode_threads":65}})", R"({"workers":{"media_threads":1.5}})",
        R"({"workers":{"media_queue_len":0}})", R"({"workers":{"endpoint_queue_len":65537}})",
        R"({"workers":{"transcode_queue_len":-1}})", R"({"workers":{"unknown":1}})",
        R"({"recording":{"worker_count":65}})"}) {
        bool rejected = false;
        try { config::AppConfig::FromJson(invalid); } catch (const std::exception&) { rejected = true; }
        Check(rejected, "invalid worker configuration accepted");
    }
}

void AccountConfiguration() {
    const auto defaults = config::AppConfig::FromJson("{}");
    Check(!defaults.account.enabled && !defaults.account.registration_enabled && !defaults.http.enabled,
          "account API and self-registration must be opt-in");
    const auto configured = config::AppConfig::FromJson(R"({
        "account": {"enabled":true,"registration_enabled":true,"database_path":"data/accounts.db","session_ttl_seconds":3600,
                    "worker_count":3,"max_pending_requests":32},
        "http": {"enabled":true,"listen_ip":"127.0.0.1","listen_port":18081,"max_body_bytes":4096,
                 "max_connections":64,"max_header_bytes":4096,"max_response_bytes":8192,
                 "max_total_response_bytes":32768,"request_timeout_seconds":10,
                 "idle_timeout_seconds":20,"service_interval_ms":2}
    })");
    Check(configured.account.enabled && configured.account.registration_enabled && configured.account.database_path == "data/accounts.db" &&
          configured.account.session_ttl_seconds == 3600 && configured.account.worker_count == 3 &&
          configured.account.max_pending_requests == 32 && configured.http.enabled &&
          configured.http.listen_ip == "127.0.0.1" && configured.http.listen_port == 18081 &&
          configured.http.max_body_bytes == 4096 && configured.http.max_connections == 64 &&
          configured.http.max_header_bytes == 4096 && configured.http.max_response_bytes == 8192 &&
          configured.http.max_total_response_bytes == 32768 && configured.http.request_timeout_seconds == 10 &&
          configured.http.idle_timeout_seconds == 20 && configured.http.service_interval_ms == 2,
          "account/HTTP configuration was not applied");
    for (const char* invalid : {
        R"({"http":{"enabled":true}})",
        R"({"account":{"enabled":"true"}})",
        R"({"account":{"registration_enabled":"true"}})",
        R"({"account":{"database_path":""}})",
        R"({"account":{"session_ttl_seconds":0}})",
        R"({"account":{"worker_count":0}})", R"({"account":{"worker_count":65}})",
        R"({"account":{"max_pending_requests":-1}})", R"({"account":{"max_pending_requests":65537}})",
        R"({"account":{"unknown":true}})", R"({"http":{"enabled":1}})",
        R"({"http":{"listen_port":65536}})", R"({"http":{"max_body_bytes":0}})",
        R"({"http":{"max_body_bytes":1048577}})", R"({"http":{"listen_ip":""}})",
        R"({"http":{"max_connections":0}})", R"({"http":{"max_header_bytes":0}})",
        R"({"http":{"max_response_bytes":0}})", R"({"http":{"max_total_response_bytes":1024}})",
        R"({"http":{"request_timeout_seconds":0}})", R"({"http":{"idle_timeout_seconds":3601}})",
        R"({"http":{"service_interval_ms":0}})", R"({"http":{"unknown":true}})"}) {
        bool rejected = false;
        try { config::AppConfig::FromJson(invalid); } catch (const std::exception&) { rejected = true; }
        Check(rejected, "invalid account/HTTP configuration accepted");
    }
    auto invalid = defaults;
    invalid.http.enabled = true;
    bool rejected = false;
    try { invalid.Validate(); } catch (const std::invalid_argument&) { rejected = true; }
    Check(rejected, "programmatic HTTP configuration must require accounts");
}

void StreamConfiguration() {
    auto config = config::AppConfig::FromJson(R"({
        "services": {"recording":true,"ai":false,"conference_mix":true},
        "stream_defaults": {"recording":false,"ai":true,"conference_mix":true},
        "streams": [
          {"session_id":"room-a","stream_id":"camera","recording":true,"ai":true},
          {"session_id":"room-b","stream_id":"camera","conference_mix":false}
        ]
    })");
    config::ConfigStore store(config);
    const auto old = store.Snapshot();
    auto alice = store.Resolve({"room-a", "camera"});
    auto bob = store.Resolve({"room-b", "camera"});
    Check(alice.recording && !alice.ai && alice.conference_mix, "global gate must dominate a stream override");
    Check(!bob.recording && !bob.conference_mix, "same stream ID in another session must stay isolated");
    Check(!store.Resolve({"new", "stream"}).recording, "new streams must inherit defaults");
    store.SetStreamConfig({"room-a", "camera"}, {false, std::nullopt, false});
    Check(!store.Resolve({"room-a", "camera"}).recording && old->Resolve({"room-a", "camera"}).recording,
          "updates must preserve existing immutable snapshots");
    store.RemoveStreamConfig({"room-b", "camera"});
    Check(store.Resolve({"room-b", "camera"}).conference_mix, "removal must restore inheritance");
    store.SetStreamDefaults({true, false, false});
    Check(store.Resolve({"room-b", "camera"}).recording && !store.Resolve({"room-a", "camera"}).recording,
          "defaults must not override explicit false");
    store.SetServiceEnabled(config::Feature::Recording, false);
    Check(!store.Resolve({"room-b", "camera"}).recording, "global disable");
    store.SetServiceEnabled(config::Feature::Recording, true);
    Check(!store.Resolve({"room-a", "camera"}).recording, "reenable must retain individual overrides");
    const auto before_invalid = store.Snapshot();
    bool rejected = false;
    try { store.SetStreamConfig({"", "camera"}, {}); } catch (const std::invalid_argument&) { rejected = true; }
    Check(rejected && store.Snapshot() == before_invalid, "invalid updates must be atomic");

    for (const char* invalid : {
        R"({"services":{"recording":"false"}})",
        R"({"stream_defaults":{"ai":null}})",
        R"({"streams":[{"session_id":"a","stream_id":"b"},{"session_id":"a","stream_id":"b"}]})",
        R"({"streams":[{"session_id":"","stream_id":"b"}]})",
        R"({"server":{"rtsp_port":65536}})", R"({"server":{"io_threads":-1}})",
        R"({"recording":{"worker_count":0}})", R"({"recording":{"reorder_ms":5001}})",
        R"({"recording":{"segment_ms":18446744073709551615}})", R"({"stream_default":{}})",
        R"({"webrtc":{"enabled":"true"}})", R"({"webrtc":{"max_sessions":0}})",
        R"({"webrtc":{"reconnect_timeout_ms":0}})", R"({"webrtc":{"reconnect_timeout_ms":120001}})",
        R"({"webrtc":{"admission_timeout_ms":0}})", R"({"webrtc":{"admission_timeout_ms":120001}})",
        R"({"webrtc":{"signaling_ping_seconds":0}})",
        R"({"webrtc":{"signaling_ping_seconds":60,"signaling_idle_timeout_seconds":60}})",
        R"({"webrtc":{"signaling_idle_timeout_seconds":1}})",
        R"({"webrtc":{"signaling_idle_timeout_seconds":65536}})",
        R"({"webrtc":{"public_ip":42}})", R"({"webrtc":{"unknown":true}})",
        R"({"webrtc":{"token":"old-shared-token"}})", R"({"webrtc":{"api_keys":[]}})",
        R"({"webrtc":{"api_keys":{"key":42}}})", R"({"webrtc":{"api_keys":{"key":"short"}}})",
        R"({"webrtc":{"api_keys":{"":"01234567890123456789012345678901"}}})",
        R"({"webrtc":{"api_keys":{"bad key":"01234567890123456789012345678901"}}})",
        R"({"turn":{"enabled":"true"}})", R"({"turn":{"dual_stack":1}})",
        R"({"turn":{"unauthorized_ratelimit":1}})",
        R"({"turn":{"unauthorized_ratelimit_rps":0}})",
        R"({"turn":{"unauthorized_ratelimit_rps":-1}})",
        R"({"turn":{"unauthorized_ratelimit_rps":1.5}})",
        R"({"turn":{"unauthorized_ratelimit_rps":4294967296}})",
        R"({"turn":{"listen_port":65536}})", R"({"turn":{"max_allocations":0}})",
        R"({"turn":{"relay_port_min":60000,"relay_port_max":50000}})",
        R"({"turn":{"relay_port_min":0,"relay_port_max":50000}})",
        R"({"turn":{"enabled":true,"realm":""}})", R"({"turn":{"unknown":true}})"}) {
        rejected = false;
        try { config::AppConfig::FromJson(invalid); } catch (const std::exception&) { rejected = true; }
        Check(rejected, "invalid JSON configuration accepted");
    }
    auto inherit = config::AppConfig::FromJson(R"({"streams":[{"session_id":"a","stream_id":"b","recording":null}]})");
    Check(inherit.Resolve({"a", "b"}).recording, "null must mean inherit for stream overrides");
    auto unsliced = config::AppConfig::FromJson(R"({"recording":{"segment_ms":0,"discovery_ms":0}})");
    Check(unsliced.recording.segment_ms == 0 && unsliced.recording.discovery_ms == 0, "zero timing options must preserve recorder semantics");
    auto sliced = config::AppConfig::FromJson(R"({"recording":{"segment_ms":1000,"reorder_ms":50,"index_path":"index/recordings.sqlite"}})");
    Check(sliced.recording.segment_ms == 1000 && sliced.recording.reorder_ms == 50 &&
          sliced.recording.index_path == "index/recordings.sqlite", "segment/index configuration was not applied");
    auto rtc = config::AppConfig::FromJson(R"({"webrtc":{"enabled":true,"public_ip":"192.0.2.1","api_keys":{"test-key":"test-only-secret-with-32-bytes-minimum"},"max_sessions":12}})");
    Check(rtc.webrtc.enabled && rtc.webrtc.public_ip == "192.0.2.1" &&
          rtc.webrtc.api_keys.at("test-key") == "test-only-secret-with-32-bytes-minimum" && rtc.webrtc.max_sessions == 12,
          "WebRTC configuration was not applied");
    for (const auto& keys : {
        std::map<std::string, std::string>{{std::string(129, 'k'), std::string(32, 's')}},
        std::map<std::string, std::string>{{"key", std::string(4097, 's')}},
        std::map<std::string, std::string>{{"key", std::string(31, 's')}}}) {
        auto invalid = config::AppConfig{};
        invalid.webrtc.api_keys = keys;
        rejected = false;
        try { invalid.Validate(); } catch (const std::invalid_argument&) { rejected = true; }
        Check(rejected, "invalid API key bounds accepted programmatically");
    }
    Check(rtc.webrtc.reconnect_timeout_ms == 15000 &&
          config::AppConfig::FromJson(R"({"webrtc":{"reconnect_timeout_ms":5000}})").webrtc.reconnect_timeout_ms == 5000,
          "WebRTC reconnect timeout was not applied");
    const auto signaling = config::AppConfig::FromJson(R"({"webrtc":{
        "admission_timeout_ms":500,"signaling_ping_seconds":2,"signaling_idle_timeout_seconds":5}})");
    Check(rtc.webrtc.admission_timeout_ms == 10000 && rtc.webrtc.signaling_ping_seconds == 30 &&
          rtc.webrtc.signaling_idle_timeout_seconds == 60 && signaling.webrtc.admission_timeout_ms == 500 &&
          signaling.webrtc.signaling_ping_seconds == 2 && signaling.webrtc.signaling_idle_timeout_seconds == 5,
          "WebRTC signaling deadlines were not applied");
    auto turn = config::AppConfig::FromJson(R"({"turn":{
        "enabled":true,"listen_ip":"::","listen_port":13478,"dual_stack":true,
        "relay_bind_ip":"0.0.0.0","advertised_ip":"192.0.2.1",
        "relay_bind_ip_v6":"::","advertised_ip_v6":"2001:db8::1","realm":"example.org",
        "relay_port_min":0,"relay_port_max":0,"max_sessions":64,"max_allocations":32,
        "max_allocations_per_user":4,"max_permissions":16,"max_channels":8,"local_test":true,
        "unauthorized_ratelimit":true,"unauthorized_ratelimit_rps":7
    }})");
    Check(turn.turn.enabled && turn.turn.dual_stack && turn.turn.listen_ip == "::" &&
          turn.turn.listen_port == 13478 && turn.turn.relay_bind_ip == "0.0.0.0" &&
          turn.turn.advertised_ip == "192.0.2.1" && turn.turn.relay_bind_ip_v6 == "::" &&
          turn.turn.advertised_ip_v6 == "2001:db8::1" && turn.turn.realm == "example.org" &&
          turn.turn.relay_port_min == 0 && turn.turn.relay_port_max == 0 && turn.turn.max_sessions == 64 &&
          turn.turn.max_allocations == 32 && turn.turn.max_allocations_per_user == 4 &&
          turn.turn.max_permissions == 16 && turn.turn.max_channels == 8 && turn.turn.local_test,
          "TURN configuration was not applied");
    Check(turn.turn.unauthorized_ratelimit && turn.turn.unauthorized_ratelimit_rps == 7 &&
          !config::AppConfig{}.turn.unauthorized_ratelimit &&
          config::AppConfig{}.turn.unauthorized_ratelimit_rps == 10,
          "TURN rate-limit configuration/defaults were not applied");
    auto invalid_limit = config::AppConfig{};
    invalid_limit.turn.unauthorized_ratelimit_rps = 0;
    rejected = false;
    try { invalid_limit.Validate(); } catch (const std::invalid_argument&) { rejected = true; }
    Check(rejected, "zero TURN rate-limit threshold accepted programmatically");

    config::ConfigStore concurrent;
    std::atomic<bool> done{false}, coherent{true};
    std::thread reader([&] {
        while (!done.load()) {
            const auto effective = concurrent.Resolve({"live", "camera"});
            if (effective.recording != effective.ai) coherent = false;
        }
    });
    for (int i = 0; i < 100; ++i) concurrent.SetStreamDefaults({i % 2 == 0, i % 2 == 0, true});
    done = true;
    reader.join();
    Check(coherent, "readers observed a partial configuration update");
}
}

int main() {
    try {
        AccountConfiguration();
        SavedEnvironment recording("PACKETIA_RECORDING"), ai("PACKETIA_AI"),
                         mix("PACKETIA_CONFERENCE_MIX"), directory("PACKETIA_RECORD_DIR"), path("PACKETIA_CONFIG"),
                         rtc("PACKETIA_WEBRTC"), rtc_ip("PACKETIA_WEBRTC_PUBLIC_IP"), rtc_token("PACKETIA_WEBRTC_TOKEN"),
                         rtc_key("PACKETIA_WEBRTC_API_KEY"), rtc_secret("PACKETIA_WEBRTC_API_SECRET"),
                         turn("PACKETIA_TURN");
        auto config = server::ServerConfig::FromEnvironment();
        Check(config.recording_enabled && config.ai_enabled && !config.conference_mix_enabled, "defaults changed");
        Check(!config.webrtc.enabled, "WebRTC must be opt-in");
        Check(!config.turn.enabled, "TURN must be opt-in");
        Set("PACKETIA_TURN", "true");
        config = server::ServerConfig::FromEnvironment();
        Check(config.turn.enabled, "TURN environment override ignored");
        Set("PACKETIA_TURN", "false");
        config = server::ServerConfig::FromEnvironment();
        Check(!config.turn.enabled, "TURN disable ignored");
        Set("PACKETIA_TURN", "invalid");
        bool turn_rejected = false;
        try { server::ServerConfig::FromEnvironment(); }
        catch (const std::invalid_argument&) { turn_rejected = true; }
        Check(turn_rejected, "invalid TURN flag accepted");
        Set("PACKETIA_TURN", nullptr);
        Set("PACKETIA_WEBRTC", "true");
        Set("PACKETIA_WEBRTC_PUBLIC_IP", "192.0.2.2");
        Set("PACKETIA_WEBRTC_API_KEY", "environment-key");
        Set("PACKETIA_WEBRTC_API_SECRET", "environment-test-secret-at-least-32-bytes");
        config = server::ServerConfig::FromEnvironment();
        Check(config.webrtc.enabled && config.webrtc.public_ip == "192.0.2.2" &&
              config.webrtc.api_keys.at("environment-key") == "environment-test-secret-at-least-32-bytes",
              "WebRTC environment overrides ignored");
        Set("PACKETIA_WEBRTC_TOKEN", "old-token");
        bool old_token_rejected = false;
        try { server::ServerConfig::FromEnvironment(); }
        catch (const std::invalid_argument&) { old_token_rejected = true; }
        Check(old_token_rejected, "legacy shared token environment must fail with a migration error");
        Set("PACKETIA_WEBRTC_TOKEN", nullptr);
        Set("PACKETIA_WEBRTC_API_SECRET", nullptr);
        bool partial_key_rejected = false;
        try { server::ServerConfig::FromEnvironment(); }
        catch (const std::invalid_argument&) { partial_key_rejected = true; }
        Check(partial_key_rejected, "an API key without its secret must be rejected");
        Set("PACKETIA_WEBRTC_API_SECRET", "environment-test-secret-at-least-32-bytes");
        for (const char* disabled : {"0", "false", "OFF", "No"}) {
            Set("PACKETIA_RECORDING", disabled);
            Set("PACKETIA_AI", disabled);
            Set("PACKETIA_CONFERENCE_MIX", disabled);
            config = server::ServerConfig::FromEnvironment();
            Check(!config.recording_enabled && !config.ai_enabled && !config.conference_mix_enabled,
                  "disabled flags parsed as enabled");
        }
        for (const char* enabled : {"1", "true", "ON", "Yes"}) {
            Set("PACKETIA_RECORDING", enabled);
            Set("PACKETIA_AI", enabled);
            Set("PACKETIA_CONFERENCE_MIX", enabled);
            config = server::ServerConfig::FromEnvironment();
            Check(config.recording_enabled && config.ai_enabled && config.conference_mix_enabled,
                  "enabled flags parsed as disabled");
        }
        Set("PACKETIA_AI", "false");
        Set("PACKETIA_RECORD_DIR", "example-recordings");
        config = server::ServerConfig::FromEnvironment();
        Check(config.recording_enabled && !config.ai_enabled && config.conference_mix_enabled &&
              config.recording.directory == "example-recordings", "switches must remain independent");
        Set("PACKETIA_AI", "invalid");
        bool rejected = false;
        try { server::ServerConfig::FromEnvironment(); }
        catch (const std::invalid_argument&) { rejected = true; }
        Check(rejected, "invalid flags must not silently enable a feature");
        StreamConfiguration();
        WorkerConfiguration();
        const auto file = std::filesystem::temp_directory_path() / "packetia-test-config.json";
        { std::ofstream output(file); output << R"({"services":{"recording":false,"ai":true},"streams":[{"session_id":"a","stream_id":"b","recording":true}]})"; }
        Set("PACKETIA_CONFIG", file.string().c_str());
        Set("PACKETIA_RECORDING", nullptr);
        Set("PACKETIA_AI", "false");
        config = server::ServerConfig::FromEnvironment();
        std::filesystem::remove(file);
        Check(!config.recording_enabled && !config.ai_enabled && config.streams.size() == 1,
              "file must override defaults and environment must override file");
        std::cout << "Server configuration tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
