#include "service/RecordService/RecordingService.h"
#include "service/AIService/AIService.h"
#include "service/AIService/UnavailableModelProvider.h"
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <stdexcept>

using namespace std::chrono_literals;
namespace {
void Check(bool result, const char* message) { if (!result) throw std::runtime_error(message); }
media::EncodedFrameEvent Frame(const char* session) {
    auto frame = std::make_shared<media::EncodedFrame>();
    frame->info.track_id = 1;
    frame->info.media_type = media::MediaType::Video;
    frame->info.codec = media::CodecType::H264;
    frame->buffer = common::SharedBuffer::Take(std::vector<uint8_t>{1, 2, 3});
    media::EncodedFrameEvent event;
    event.source.endpoint_id = 1;
    event.source.track_id = 1;
    event.source.session_id = session;
    event.source.stream_id = "camera";
    event.frame = frame;
    return event;
}
class RecordEvents final : public service::IRecordingEventSink {
public:
    std::mutex mutex;
    std::condition_variable ready;
    std::vector<service::RecordingEvent> events;
    void OnRecordingEvent(const service::RecordingEvent& event) override {
        std::lock_guard<std::mutex> lock(mutex);
        events.push_back(event);
        ready.notify_all();
    }
    size_t Count(const char* session, service::RecordingEventType type) const {
        size_t count = 0;
        for (const auto& event : events) if (event.instance.session.session_id == session && event.type == type) ++count;
        return count;
    }
    bool Wait(const char* session, service::RecordingEventType type, size_t count) {
        std::unique_lock<std::mutex> lock(mutex);
        return ready.wait_for(lock, 3s, [&] { return Count(session, type) >= count; });
    }
};
class Processor final : public service::ai::IAIFrameProcessor {
public:
    std::mutex mutex;
    std::condition_variable ready;
    size_t count = 0;
    void Process(const media::EncodedFrameEvent&) override {
        std::lock_guard<std::mutex> lock(mutex);
        ++count;
        ready.notify_all();
    }
    bool Wait(size_t expected) {
        std::unique_lock<std::mutex> lock(mutex);
        return ready.wait_for(lock, 3s, [&] { return count == expected; });
    }
};

void RecordingPolicies() {
    auto router = std::make_shared<media::EncodedFrameRouter>();
    auto settings = std::make_shared<config::ConfigStore>();
    auto events = std::make_shared<RecordEvents>();
    service::RecordingOptions options;
    const auto directory = std::filesystem::temp_directory_path() /
        ("packetia-policy-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    options.directory = directory.string();
    // Session closure must be caused by policy, not idle timeout.
    options.idle_timeout_ms = 60000;
    auto recording = std::make_shared<service::RecordingService>(router, options, events, settings);
    Check(recording->Init() && recording->Start(), "recording startup");
    Check(router->Publish(Frame("alice")) == 1 && router->Publish(Frame("bob")) == 1, "both streams admitted");
    Check(events->Wait("alice", service::RecordingEventType::SessionRecording, 1), "alice recording");
    Check(events->Wait("bob", service::RecordingEventType::SessionRecording, 1), "bob recording");
    settings->SetStreamConfig({"alice", "camera"}, {false, std::nullopt, std::nullopt});
    recording->RefreshStreamPolicies();
    Check(router->Publish(Frame("alice")) == 0, "disabled stream admission");
    Check(events->Wait("alice", service::RecordingEventType::SessionStopped, 1), "close without waiting for idle");
    {
        std::lock_guard<std::mutex> lock(events->mutex);
        Check(events->Count("bob", service::RecordingEventType::SessionStopped) == 0, "other stream was interrupted");
        bool policy_reason = false;
        for (const auto& event : events->events)
            policy_reason |= event.instance.session.session_id == "alice" &&
                             event.reason == service::RecordingStopReason::ConfigurationChanged;
        Check(policy_reason, "missing configuration stop reason");
    }
    settings->RemoveStreamConfig({"alice", "camera"});
    recording->RefreshStreamPolicies();
    Check(router->Publish(Frame("alice")) == 1, "reenabled recording admission");
    Check(events->Wait("alice", service::RecordingEventType::SessionRecording, 2), "new recording session");
    // A fast off/on still creates a file/session boundary, even between ticks.
    settings->SetStreamConfig({"alice", "camera"}, {false, {}, {}});
    recording->RefreshStreamPolicies();
    settings->RemoveStreamConfig({"alice", "camera"});
    recording->RefreshStreamPolicies();
    Check(router->Publish(Frame("alice")) == 1, "fast reenable admission");
    Check(events->Wait("alice", service::RecordingEventType::SessionRecording, 3), "fast toggle must rotate session");
    settings->SetStreamConfig({"alice", "camera"}, {true, false, true});
    Check(recording->StopRecording({"alice", "camera"}), "explicit stop must wait for completion");
    Check(router->Publish(Frame("alice")) == 0, "explicit stop must close admission");
    Check(recording->StopRecording({"alice", "camera"}), "explicit stop must be idempotent");
    Check(recording->StartRecording({"alice", "camera"}), "explicit restart");
    const auto override = settings->Snapshot()->streams.at({"alice", "camera"});
    Check(override.ai == false && override.conference_mix == true, "recording controls changed unrelated options");
    Check(router->Publish(Frame("alice")) == 1, "explicit restart must admit immediately");
    Check(recording->StopRecording({"alice", "camera"}), "stop immediately after submitting a frame");
    Check(events->Wait("alice", service::RecordingEventType::SessionStopped, 4), "admitted frame must drain before stop returns");
    recording->Stop();
    {
        std::lock_guard<std::mutex> lock(events->mutex);
        Check(events->Count("bob", service::RecordingEventType::SessionStarted) == 1, "bob session should not restart");
    }
    recording.reset(); // Close the real index; media persistence remains stubbed.
    std::filesystem::remove_all(directory);
}

void AIPolicies() {
    auto router = std::make_shared<media::EncodedFrameRouter>();
    config::AppConfig initial;
    initial.stream_defaults.ai = false;
    initial.streams[{"alice", "camera"}].ai = true;
    auto settings = std::make_shared<config::ConfigStore>(initial);
    auto processor = std::make_shared<Processor>();
    service::ai::AIService ai(std::make_shared<service::ai::UnavailableModelProvider>(), router, processor, 128, settings);
    Check(ai.Init() && ai.Start(), "AI startup");
    Check(router->Publish(Frame("alice")) == 1 && router->Publish(Frame("bob")) == 0, "AI per-stream admission");
    Check(processor->Wait(1), "AI selected stream not processed");
    settings->SetStreamConfig({"bob", "camera"}, {{}, true, {}});
    Check(router->Publish(Frame("bob")) == 1 && processor->Wait(2), "AI stream reenable");
    settings->SetServiceEnabled(config::Feature::AI, false);
    Check(router->Publish(Frame("alice")) == 0 && router->Publish(Frame("bob")) == 0, "AI global gate bypassed");
    ai.Stop();
}
}
int main() {
    try {
        RecordingPolicies();
        AIPolicies();
        std::cout << "Stream service policy tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
