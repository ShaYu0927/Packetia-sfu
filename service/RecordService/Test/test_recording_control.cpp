#include "RecordingFixture.h"
#include "RecordingService.h"
#include <mutex>

using namespace recording_test;
namespace {
class Events final : public service::IRecordingEventSink {
public:
    void OnRecordingEvent(const service::RecordingEvent& event) override {
        std::lock_guard<std::mutex> lock(mutex_);
        events_.push_back(event);
    }
    std::vector<service::RecordingEvent> Find(const std::string& session, service::RecordingEventType type) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<service::RecordingEvent> result;
        for (const auto& event : events_)
            if (event.instance.session.session_id == session && event.type == type) result.push_back(event);
        return result;
    }
private:
    std::mutex mutex_;
    std::vector<service::RecordingEvent> events_;
};

std::vector<TimedFrame> Published(const std::vector<TimedFrame>& fixture, const char* session,
                                uint64_t endpoint, int64_t offset_us) {
    auto frames = fixture;
    for (auto& value : frames) {
        value.event.source.session_id = session;
        value.event.source.stream_id = "camera";
        value.event.source.endpoint_id = endpoint;
        auto frame = std::make_shared<media::EncodedFrame>(*value.event.frame);
        auto& timestamp = frame->info.timestamp;
        timestamp.pts += av_rescale_q(offset_us, AVRational{1, 1000000},
            AVRational{timestamp.time_base_num, timestamp.time_base_den});
        timestamp.dts = timestamp.pts;
        timestamp.receive_time_ms += offset_us / 1000;
        timestamp.capture_time_ms += offset_us / 1000;
        value.event.frame = std::move(frame);
        value.time_us += offset_us;
    }
    return frames;
}

void TestControl(const std::vector<TimedFrame>& fixture, const std::filesystem::path& directory) {
    config::AppConfig config;
    config.stream_defaults.recording = false;
    config.recording.directory = directory.string();
    // All three recordings stop before discovery expires: Stop must force-open
    // buffered H264/AAC, drain pending packets and write a playable trailer.
    config.recording.discovery_ms = 60000;
    config.recording.idle_timeout_ms = 60000;
    config.recording.segment_ms = 0;
    config.streams[{"zhangsan", "camera"}] = {false, true, false};
    auto settings = std::make_shared<config::ConfigStore>(config);
    auto router = std::make_shared<media::EncodedFrameRouter>();
    auto events = std::make_shared<Events>();
    auto recording = std::make_shared<service::RecordingService>(router, config.recording, events, settings);
    const config::StreamKey zhangsan{"zhangsan", "camera"}, lisi{"lisi", "camera"};
    CHECK(!recording->StartRecording(zhangsan));
    CHECK(recording->Init() && recording->Start());
    CHECK(!recording->StartRecording({"", "camera"}));
    CHECK(recording->StopRecording({"not-published", "camera"}));
    settings->SetServiceEnabled(config::Feature::Recording, false);
    CHECK(!recording->StartRecording(zhangsan));
    settings->SetServiceEnabled(config::Feature::Recording, true);
    CHECK(recording->StartRecording(zhangsan) && recording->StartRecording(lisi));
    CHECK(recording->StartRecording(zhangsan)); // Idempotent, no extra generation.
    const auto overrides = settings->Snapshot()->streams.at(zhangsan);
    CHECK(overrides.ai == true && overrides.conference_mix == false);

    auto z0 = Published(fixture, "zhangsan", 101, 0);
    auto l0 = Published(fixture, "lisi", 102, 0);
    for (size_t i = 0; i < z0.size(); ++i) {
        CHECK(router->Publish(z0[i].event) == 1);
        CHECK(router->Publish(l0[i].event) == 1);
    }
    CHECK(recording->StopRecording(zhangsan)); // No sleep or wait-for-stats before stopping.
    CHECK(recording->StopRecording(zhangsan));
    auto completed = events->Find("zhangsan", service::RecordingEventType::SegmentCompleted);
    CHECK(completed.size() == 1);
    VerifyMp4(completed[0].path, z0);
    CHECK(events->Find("lisi", service::RecordingEventType::SessionStopped).empty());

    auto z1 = Published(fixture, "zhangsan", 101, 2000000);
    auto l1 = Published(fixture, "lisi", 102, 2000000);
    for (size_t i = 0; i < z1.size(); ++i) {
        CHECK(router->Publish(z1[i].event) == 0);
        CHECK(router->Publish(l1[i].event) == 1);
    }
    CHECK(recording->StartRecording(zhangsan));
    auto z2 = Published(fixture, "zhangsan", 101, 4000000);
    auto l2 = Published(fixture, "lisi", 102, 4000000);
    for (size_t i = 0; i < z2.size(); ++i) {
        CHECK(router->Publish(z2[i].event) == 1);
        CHECK(router->Publish(l2[i].event) == 1);
    }
    CHECK(recording->StopRecording(zhangsan));
    completed = events->Find("zhangsan", service::RecordingEventType::SegmentCompleted);
    CHECK(completed.size() == 2 && completed[0].path != completed[1].path);
    CHECK(completed[0].instance.generation != completed[1].instance.generation);
    VerifyMp4(completed[0].path, z0); // First file was not overwritten or appended.
    VerifyMp4(completed[1].path, z2);
    CHECK(recording->StopRecording(lisi));
    auto li_completed = events->Find("lisi", service::RecordingEventType::SegmentCompleted);
    CHECK(li_completed.size() == 1);
    l0.insert(l0.end(), l1.begin(), l1.end());
    l0.insert(l0.end(), l2.begin(), l2.end());
    VerifyMp4(li_completed[0].path, l0);
    const auto stopped = events->Find("zhangsan", service::RecordingEventType::SessionStopped);
    CHECK(stopped.size() == 2 && stopped.back().reason == service::RecordingStopReason::UserRequested);
    CHECK(recording->Stats().written == fixture.size() * 5);
    CHECK(recording->Stats().dropped == 0 && recording->Stats().errors == 0);

    // An undecodable short recording must not be reported as a successful file.
    CHECK(recording->StartRecording({"missing-keyframe", "camera"}));
    auto delta = std::find_if(z0.begin(), z0.end(), [](const TimedFrame& value) {
        return value.event.frame->frame_type == media::EncodedFrameType::Delta;
    });
    CHECK(delta != z0.end());
    auto invalid = delta->event;
    invalid.source.session_id = "missing-keyframe";
    CHECK(router->Publish(invalid) == 1);
    CHECK(!recording->StopRecording({"missing-keyframe", "camera"}));
    CHECK(events->Find("missing-keyframe", service::RecordingEventType::SessionFailed).size() == 1);
    CHECK(events->Find("missing-keyframe", service::RecordingEventType::SegmentCompleted).empty());
    recording->Stop();
    CHECK(router->Publish(z2.front().event) == 0);
    size_t files = 0;
    for (const auto& file : std::filesystem::directory_iterator(directory)) {
        if (file.path().filename() == ".index") continue;
        CHECK(file.path().extension() == ".mp4" && file.path().filename().string().front() != '.');
        ++files;
    }
    CHECK(files == 3);
    for (const auto& file : completed) std::cout << "zhangsan: " << file.path << '\n';
    std::cout << "lisi: " << li_completed[0].path << '\n';
}
}

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    try {
        const std::filesystem::path directory(argv[2]);
        std::filesystem::create_directories(directory);
        CHECK(std::filesystem::is_empty(directory));
        auto frames = LoadFixture(argv[1]);
        Order(frames, false);
        TestControl(frames, directory);
        std::cout << "Passed: independent stream start/stop/restart, exact packet counts, finalization and failure reporting\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Recording control regression failed: " << error.what() << "\nArtifacts: " << argv[2] << '\n';
        return 1;
    }
}
