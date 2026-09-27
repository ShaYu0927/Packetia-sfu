#include "RecordingFixture.h"
#include "RecordingCatalog.h"
#include "RecordingService.h"
#include <fstream>
#include <set>
#include <sqlite3.h>

using namespace recording_test;
namespace {
std::vector<TimedFrame> Frames(const std::vector<TimedFrame>& fixture, const char* stream,
                               uint64_t endpoint, int64_t offset = 0) {
    auto result = fixture;
    for (auto& f : result) {
        f.event.source.session_id = "meeting-A";
        f.event.source.stream_id = stream;
        f.event.source.endpoint_id = endpoint;
        auto frame = std::make_shared<media::EncodedFrame>(*f.event.frame);
        auto& t = frame->info.timestamp;
        t.pts += av_rescale_q(offset, {1, 1000000}, {t.time_base_num, t.time_base_den});
        t.dts = t.pts;
        t.capture_time_ms += offset / 1000;
        t.receive_time_ms += offset / 1000;
        f.time_us += offset;
        f.event.frame = frame;
    }
    return result;
}

// Compare every packet timestamp and payload, not only output packet counts.
// Annex B conversion restores the representation used at recorder ingress.
void VerifySegments(const std::filesystem::path& root, const std::vector<service::SegmentInfo>& segments,
                    std::vector<TimedFrame> expected, std::ofstream& manifest) {
    CHECK(!segments.empty());
    Order(expected, false);
    const int64_t input_origin = expected.front().time_us;
    std::vector<TimedFrame> actual;
    uint64_t sequence = 0;
    for (const auto& segment : segments) {
        CHECK(segment.sequence == ++sequence);
        CHECK(segment.status == service::SegmentStatus::Completed);
        CHECK(segment.session_id == "meeting-A" && segment.tracks.size() == 2);
        CHECK(segment.media_end_us > segment.media_start_us);
        const auto path = root / segment.relative_path;
        CHECK(segment.size_bytes == std::filesystem::file_size(path));
        auto packets = LoadFixture(path.string().c_str());
        auto video = std::find_if(packets.begin(), packets.end(), [](const TimedFrame& f) {
            return f.event.frame->info.media_type == media::MediaType::Video;
        });
        CHECK(video != packets.end() && video->event.frame->IsKeyFrame());
        for (auto& p : packets) p.time_us += segment.media_start_us + input_origin;
        actual.insert(actual.end(), packets.begin(), packets.end());
        manifest << path.string() << '\n';
    }
    Order(actual, false);
    CHECK(actual.size() == expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        CHECK(actual[i].event.frame->info.codec == expected[i].event.frame->info.codec);
        // AAC muxer timescale rounding is bounded to one sample.
        CHECK(std::abs(actual[i].time_us - expected[i].time_us) <= 25);
        CHECK(actual[i].event.frame->buffer.ToVector() == expected[i].event.frame->buffer.ToVector());
    }
}

void TestService(const std::vector<TimedFrame>& fixture, const std::filesystem::path& root, std::ofstream& manifest) {
    config::AppConfig cfg;
    cfg.recording.directory = root.string();
    cfg.recording.segment_ms = 1000;
    cfg.recording.discovery_ms = 60000; // All media queued fast: cut by media time, never worker elapsed time.
    cfg.recording.idle_timeout_ms = 60000;
    cfg.stream_defaults.recording = false;
    auto config = std::make_shared<config::ConfigStore>(cfg);
    auto router = std::make_shared<media::EncodedFrameRouter>();
    auto recorder = std::make_shared<service::RecordingService>(router, cfg.recording, nullptr, config);
    CHECK(recorder->Init() && recorder->Start());
    const config::StreamKey a{"meeting-A", "zhangsan-camera"}, b{"meeting-A", "lisi-camera"};
    CHECK(recorder->StartRecording(a) && recorder->StartRecording(b));
    auto a0 = Frames(fixture, a.stream_id.c_str(), 101);
    auto b0 = Frames(fixture, b.stream_id.c_str(), 102);
    for (size_t i = 0; i < a0.size(); ++i) {
        CHECK(router->Publish(a0[i].event) == 1);
        CHECK(router->Publish(b0[i].event) == 1);
    }
    CHECK(recorder->StopRecording(a));
    service::SegmentQuery q;
    q.session_id = "meeting-A"; q.stream_id = a.stream_id;
    auto a_first = recorder->QuerySegments(q);
    CHECK(a_first.size() == 2);
    VerifySegments(root, a_first, a0, manifest);
    for (auto& f : Frames(fixture, b.stream_id.c_str(), 102, 2000000)) {
        CHECK(router->Publish(f.event) == 1);
        b0.push_back(f);
    }
    CHECK(router->Publish(a0.front().event) == 0);
    CHECK(recorder->StartRecording(a));
    auto a2 = Frames(fixture, a.stream_id.c_str(), 101, 4000000);
    auto b2 = Frames(fixture, b.stream_id.c_str(), 102, 4000000);
    for (size_t i = 0; i < a2.size(); ++i) {
        CHECK(router->Publish(a2[i].event) == 1);
        CHECK(router->Publish(b2[i].event) == 1);
        b0.push_back(b2[i]);
    }
    CHECK(recorder->StopRecording(a) && recorder->StopRecording(b));
    CHECK(recorder->Stats().errors == 0 && recorder->Stats().dropped == 0);
    CHECK(recorder->Stats().written == fixture.size() * 5);
    const auto streams = recorder->ListRecordedStreams("meeting-A");
    CHECK(streams.size() == 3); // Two generations for Zhangsan, one for Lisi.
    q.stream_id.clear();
    for (const auto& stream : streams) {
        if (stream.recording_id == a_first.front().recording_id) continue;
        q.recording_id = stream.recording_id;
        auto segments = recorder->QuerySegments(q);
        CHECK(segments.size() == (stream.stream_id == a.stream_id ? 2 : 6));
        VerifySegments(root, segments, stream.stream_id == a.stream_id ? a2 : b0, manifest);
    }
    recorder->Stop(); recorder.reset();
    service::RecordingCatalog reopened((root / ".index/recordings.sqlite").string());
    q.recording_id.clear();
    CHECK(reopened.Query(q).size() == 10);
    q.limit = 3; q.offset = 3;
    CHECK(reopened.Query(q).size() == 3);
    q.offset = 0; q.limit = 100;
    q.stream_id = a.stream_id; q.recording_id = a_first.front().recording_id;
    q.from_ms = a_first.front().started_at_ms; q.to_ms = q.from_ms + 500;
    CHECK(reopened.Query(q).size() == 1);
    q.session_id = "other-meeting";
    CHECK(reopened.Query(q).empty());
}

void TestLiveOrdering(const std::vector<TimedFrame>& fixture, const std::filesystem::path& root, std::ofstream& manifest) {
    std::filesystem::create_directories(root);
    service::RecordingOptions options;
    options.directory = root.string(); options.segment_ms = 1000;
    options.discovery_ms = 80; options.reorder_ms = 100;
    std::atomic<uint64_t> written{0}, dropped{0}, completed{0}, errors{0};
    service::RecordingContext context(options, 200, written, dropped, completed, errors);
    context.catalog = std::make_shared<service::RecordingCatalog>((root / ".index/recordings.sqlite").string());
    service::Mp4Recorder recorder(context, {{"meeting-A", "delayed-video"}, 200, 1});
    auto frames = Frames(fixture, "delayed-video", 201);
    auto expected = frames;
    // Only video has an RTCP capture clock in another epoch. Both tracks must
    // fall back to the common receive-time domain, not mix the two clocks.
    for (auto& f : frames) {
        auto copy = std::make_shared<media::EncodedFrame>(*f.event.frame);
        if (copy->info.media_type == media::MediaType::Video)
            copy->info.timestamp.capture_time_ms += 1700000000000LL;
        else copy->info.timestamp.capture_time_valid = false;
        f.event.frame = copy;
    }
    const auto arrival = [](const TimedFrame& f) {
        return f.time_us + (f.event.frame->info.media_type == media::MediaType::Video ? 60000 : 0);
    };
    std::stable_sort(frames.begin(), frames.end(), [&](const TimedFrame& a, const TimedFrame& b) { return arrival(a) < arrival(b); });
    for (const auto& f : frames) {
        const auto now = 1000 + arrival(f) / 1000;
        recorder.InputFrame(f.event, now);
        CHECK(!recorder.Tick(now, false));
    }
    service::SegmentQuery q; q.session_id = "meeting-A";
    CHECK(context.catalog->Query(q).size() == 1); // Query completed segment while next is still recording.
    q.completed_only = false;
    CHECK(context.catalog->Query(q).size() == 2);
    recorder.Close();
    CHECK(errors == 0 && dropped == 0 && written == fixture.size() && context.pending_bytes == 0);
    q.completed_only = true;
    VerifySegments(root, context.catalog->Query(q), expected, manifest);
}

void TestFailure(const std::vector<TimedFrame>& fixture, const std::filesystem::path& root) {
    std::filesystem::create_directories(root);
    service::RecordingOptions options; options.directory = root.string(); options.discovery_ms = 60000;
    std::atomic<uint64_t> written{0}, dropped{0}, completed{0}, errors{0};
    service::RecordingContext context(options, 300, written, dropped, completed, errors);
    context.catalog = std::make_shared<service::RecordingCatalog>((root / ".index/recordings.sqlite").string());
    service::Mp4Recorder recorder(context, {{"meeting-A", "bad-stream"}, 300, 1});
    const auto frames = Frames(fixture, "bad-stream", 301);
    const auto delta = std::find_if(frames.begin(), frames.end(), [](const TimedFrame& f) { return f.event.frame->frame_type == media::EncodedFrameType::Delta; });
    CHECK(delta != frames.end());
    recorder.InputFrame(delta->event, 1000); recorder.Close();
    CHECK(recorder.HasFailed() && errors == 1 && completed == 0);
    service::SegmentQuery q; q.session_id = "meeting-A";
    CHECK(context.catalog->Query(q).empty());
    q.completed_only = false;
    auto failed = context.catalog->Query(q);
    CHECK(failed.size() == 1 && failed.front().status == service::SegmentStatus::Failed && !failed.front().error.empty());

    // File close/rename succeeds but the completion transaction fails. The
    // segment must remain excluded from playback and the recorder must fail.
    sqlite3* fault_db = nullptr;
    CHECK(sqlite3_open((root / ".index/recordings.sqlite").string().c_str(), &fault_db) == SQLITE_OK);
    CHECK(sqlite3_exec(fault_db,
        "CREATE TRIGGER reject_completion BEFORE UPDATE OF status ON segments "
        "WHEN NEW.stream_id='index-failure' AND NEW.status=1 "
        "BEGIN SELECT RAISE(ABORT,'injected index write failure'); END;", nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(fault_db);
    service::Mp4Recorder failed_index(context, {{"meeting-A", "index-failure"}, 300, 2});
    for (const auto& f : Frames(fixture, "index-failure", 302)) failed_index.InputFrame(f.event, 2000);
    failed_index.Close();
    CHECK(failed_index.HasFailed() && errors == 2 && completed == 0 && context.pending_bytes == 0);
    q.stream_id = "index-failure"; q.completed_only = true;
    CHECK(context.catalog->Query(q).empty());
    q.completed_only = false;
    failed = context.catalog->Query(q);
    CHECK(failed.size() == 1 && failed.front().status == service::SegmentStatus::Failed);
    CHECK(std::filesystem::exists(root / failed.front().relative_path));

    // A fragment finalization error must not be reset when rotating to the next segment.
    auto rename_options = options;
    rename_options.discovery_ms = 50; rename_options.segment_ms = 1000;
    service::RecordingContext rename_context(rename_options, 300, written, dropped, completed, errors);
    rename_context.catalog = context.catalog;
    service::Mp4Recorder failed_rename(rename_context, {{"meeting-A", "rename-failure"}, 300, 3});
    bool obstructed = false;
    for (const auto& f : Frames(fixture, "rename-failure", 303)) {
        failed_rename.InputFrame(f.event, 3000 + f.time_us / 1000);
        if (failed_rename.IsOpen() && !obstructed) {
            q.stream_id = "rename-failure"; q.completed_only = false;
            const auto active = context.catalog->Query(q);
            CHECK(active.size() == 1);
            std::filesystem::create_directory(root / active.front().relative_path);
            obstructed = true;
        }
    }
    failed_rename.Close();
    CHECK(obstructed && failed_rename.HasFailed() && errors == 3 && completed == 0);
    q.completed_only = true;
    CHECK(context.catalog->Query(q).empty());
    q.completed_only = false;
    failed = context.catalog->Query(q);
    CHECK(failed.size() == 1 && failed.front().status == service::SegmentStatus::Failed);
}
}

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    try {
        auto fixture = LoadFixture(argv[1]); Order(fixture, false);
        std::filesystem::path root(argv[2]); std::filesystem::create_directories(root);
        std::ofstream manifest(root / "completed.txt");
        TestService(fixture, root / "service", manifest);
        TestLiveOrdering(fixture, root / "live", manifest);
        TestFailure(fixture, root / "failed");
        std::cout << "Passed: media-time cuts, cross-track ordering, packet identity/timestamps, independent streams, restart, persistent queries and failure exclusion\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Segment regression failed: " << e.what() << '\n';
        return 1;
    }
}
