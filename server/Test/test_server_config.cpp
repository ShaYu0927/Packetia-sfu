#include "server/ServerConfig.h"
#include "config/ConfigStore.h"

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
        R"({"webrtc":{"public_ip":42}})", R"({"webrtc":{"unknown":true}})"}) {
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
    auto rtc = config::AppConfig::FromJson(R"({"webrtc":{"enabled":true,"public_ip":"192.0.2.1","token":"test-only","max_sessions":12}})");
    Check(rtc.webrtc.enabled && rtc.webrtc.public_ip == "192.0.2.1" &&
          rtc.webrtc.token == "test-only" && rtc.webrtc.max_sessions == 12,
          "WebRTC configuration was not applied");

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
        SavedEnvironment recording("PACKETIA_RECORDING"), ai("PACKETIA_AI"),
                         mix("PACKETIA_CONFERENCE_MIX"), directory("PACKETIA_RECORD_DIR"), path("PACKETIA_CONFIG"),
                         rtc("PACKETIA_WEBRTC"), rtc_ip("PACKETIA_WEBRTC_PUBLIC_IP"), rtc_token("PACKETIA_WEBRTC_TOKEN");
        auto config = server::ServerConfig::FromEnvironment();
        Check(config.recording_enabled && config.ai_enabled && !config.conference_mix_enabled, "defaults changed");
        Check(!config.webrtc.enabled, "WebRTC must be opt-in");
        Set("PACKETIA_WEBRTC", "true");
        Set("PACKETIA_WEBRTC_PUBLIC_IP", "192.0.2.2");
        Set("PACKETIA_WEBRTC_TOKEN", "environment-test-only");
        config = server::ServerConfig::FromEnvironment();
        Check(config.webrtc.enabled && config.webrtc.public_ip == "192.0.2.2" &&
              config.webrtc.token == "environment-test-only", "WebRTC environment overrides ignored");
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
