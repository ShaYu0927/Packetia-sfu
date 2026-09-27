#include "RecordingFixture.h"

using namespace recording_test;
namespace {
void TestWriter(const std::filesystem::path& directory, const std::vector<TimedFrame>& frames) {
    const auto path = directory / "writer.mp4";
    auto file = std::make_unique<service::LocalFileIO>();
    CHECK(file->Open(path.string()) == 0);
    service::Mp4Writer writer;
    CHECK(writer.Open(std::move(file), FirstTracks(frames)));
    for (const auto& frame : frames) CHECK(writer.Write(frame.event, frame.time_us));
    CHECK(writer.Close());
    VerifyMp4(path, frames);
}

void TestRecorder(const std::filesystem::path& directory, std::vector<TimedFrame> frames,
                  bool audio_first, bool distinct_endpoints = false) {
    Order(frames, audio_first);
    if (distinct_endpoints) {
        for (auto& frame : frames) {
            if (frame.event.frame->info.media_type != media::MediaType::Audio) continue;
            frame.event.source.endpoint_id = 43;
            frame.event.source.track_id = 0; // Track IDs are local to an endpoint.
            auto copy = std::make_shared<media::EncodedFrame>(*frame.event.frame);
            copy->info.track_id = 0;
            frame.event.frame = std::move(copy);
        }
    }
    std::filesystem::create_directory(directory);
    service::RecordingOptions options;
    options.directory = directory.string();
    options.discovery_ms = 50;
    std::atomic<uint64_t> written{0}, dropped{0}, completed{0}, errors{0};
    service::RecordingContext context(options, 1, written, dropped, completed, errors);
    service::Mp4Recorder recorder(context, {{"session", "stream"}, 1, 1});
    for (const auto& frame : frames) {
        const uint64_t now = 1000 + frame.time_us / 1000;
        recorder.InputFrame(frame.event, now);
        CHECK(!recorder.Tick(now, false));
        CHECK(!recorder.HasFailed());
    }
    CHECK(recorder.IsOpen());
    recorder.Close();
    CHECK(!recorder.HasFailed());
    CHECK(written == frames.size() && dropped == 0 && completed == 1 && errors == 0);
    CHECK(context.pending_bytes == 0);
    size_t files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        CHECK(entry.path().extension() == ".mp4");
        VerifyMp4(entry.path(), frames);
        ++files;
    }
    CHECK(files == 1);
}

void TestParameterChange(const std::filesystem::path& directory,
                         const std::vector<TimedFrame>& frames) {
    service::RecordingOptions options;
    options.directory = directory.string();
    options.discovery_ms = 0;
    std::atomic<uint64_t> written{0}, dropped{0}, completed{0}, errors{0};
    service::RecordingContext context(options, 1, written, dropped, completed, errors);
    service::Mp4Recorder recorder(context, {{"session", "stream"}, 1, 1});
    auto first = FirstTracks(frames).front();
    recorder.InputFrame(first, 1000);
    CHECK(!recorder.Tick(1000, false));
    CHECK(recorder.IsOpen());
    auto changed = std::make_shared<media::EncodedFrame>(*first.frame);
    changed->video.width += 16;
    first.frame = std::move(changed);
    recorder.InputFrame(first, 1040);
    CHECK(recorder.HasFailed());
    CHECK(errors == 1 && context.pending_bytes == 0);
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const auto directory = std::filesystem::temp_directory_path() /
        ("packetia-recording-tracks-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        CHECK(std::filesystem::create_directory(directory));
        auto frames = LoadFixture(argv[1]);
        Order(frames, false);
        TestWriter(directory, frames);
        TestRecorder(directory / "video-first", frames, false);
        TestRecorder(directory / "audio-first", frames, true);
        TestRecorder(directory / "distinct-endpoints", frames, false, true);
        TestParameterChange(directory, frames);
        std::filesystem::remove_all(directory);
        std::cout << "Passed: writer routing, video-first, audio-first, distinct endpoints, parameter changes\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Recording regression failed: " << error.what()
                  << "\nArtifacts: " << directory << '\n';
        return 1;
    }
}
