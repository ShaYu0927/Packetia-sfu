#include "service/ConferenceMixService/ConferenceMixer.h"
#include "service/ConferenceMixService/ConferenceMixService.h"
#include "server/ServerLauncher.h"

#include <chrono>
#include <iostream>
#include <stdexcept>

using namespace service::mix;
using namespace std::chrono_literals;

namespace {
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

MixConfig Config() {
    MixConfig config;
    config.room_id = "room-1";
    config.output_stream_id = "composed";
    config.output_endpoint_id = 99;
    config.inputs = {{"alice", 1, 10}, {"bob", 2, 10}};
    return config;
}

media::EncodedFrameEvent Frame(uint64_t endpoint = 1) {
    auto frame = std::make_shared<media::EncodedFrame>();
    frame->info.track_id = 10;
    frame->info.media_type = media::MediaType::Video;
    frame->info.codec = media::CodecType::H264;
    // A fake backend checks routing only; these bytes are not decodable H264.
    frame->buffer = common::SharedBuffer::Take(std::vector<uint8_t>{1, 2, 3});
    media::EncodedFrameEvent event;
    event.source.endpoint_id = endpoint;
    event.source.track_id = 10;
    event.source.session_id = "publisher-session";
    event.frame = std::move(frame);
    return event;
}

struct Probe {
    std::mutex mutex;
    std::condition_variable ready;
    std::thread::id worker;
    int starts = 0, inputs = 0, ticks = 0, stops = 0;
    bool same_thread = true, block = false, release = false;
    bool fail_start = false, throw_input = false;

    void Observe() {
        if (worker == std::thread::id{}) worker = std::this_thread::get_id();
        same_thread &= worker == std::this_thread::get_id();
    }
    bool Wait(bool for_input) {
        std::unique_lock<std::mutex> lock(mutex);
        return ready.wait_for(lock, 3s, [&] { return for_input ? inputs > 0 : ticks > 0; });
    }
    void Release() {
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
        ready.notify_all();
    }
};

class FakeBackend final : public IMixBackend {
public:
    explicit FakeBackend(std::shared_ptr<Probe> probe) : probe_(std::move(probe)) {}
    bool Start(const MixConfig&, Output output) override {
        std::lock_guard<std::mutex> lock(probe_->mutex);
        probe_->Observe();
        ++probe_->starts;
        output_ = std::move(output);
        return !probe_->fail_start;
    }
    void InputFrame(const media::EncodedFrameEvent& event) override {
        {
            std::unique_lock<std::mutex> lock(probe_->mutex);
            probe_->Observe();
            ++probe_->inputs;
            probe_->ready.notify_all();
            if (probe_->block && !probe_->ready.wait_for(lock, 3s, [&] { return probe_->release; }))
                throw std::runtime_error("test gate timed out");
            if (probe_->throw_input) throw std::runtime_error("decode failure");
        }
        last_ = event.frame;
        output_(last_);
    }
    void Tick(uint64_t) override {
        std::lock_guard<std::mutex> lock(probe_->mutex);
        probe_->Observe();
        ++probe_->ticks;
        probe_->ready.notify_all();
    }
    void Stop() noexcept override {
        {
            std::lock_guard<std::mutex> lock(probe_->mutex);
            probe_->Observe();
            ++probe_->stops;
        }
        if (last_) output_(last_); // Simulate encoder flush.
        output_ = {};
    }
private:
    std::shared_ptr<Probe> probe_;
    Output output_;
    media::EncodedFrame::ConstPtr last_;
};

class Sink final : public media::IEncodedFrameSink {
public:
    bool throw_on_flush = false;
    std::function<void()> on_frame;
    std::vector<media::EncodedFrameEvent> frames;
    bool SubmitFrame(const media::EncodedFrameEvent& event) override {
        if (on_frame) on_frame();
        if (throw_on_flush && !frames.empty()) throw std::runtime_error("sink failure");
        frames.push_back(event);
        return true;
    }
};

struct Fixture {
    std::shared_ptr<media::EncodedFrameRouter> input = std::make_shared<media::EncodedFrameRouter>();
    std::shared_ptr<media::EncodedFrameRouter> output = std::make_shared<media::EncodedFrameRouter>();
    std::shared_ptr<Probe> probe = std::make_shared<Probe>();
    std::shared_ptr<Sink> sink = std::make_shared<Sink>();
    std::shared_ptr<ConferenceMixer> mixer;
    explicit Fixture(MixConfig config = Config()) {
        output->Subscribe(sink);
        mixer = std::make_shared<ConferenceMixer>(std::move(config), input, output,
                                                 std::make_unique<FakeBackend>(probe));
    }
    ~Fixture() { probe->Release(); mixer->Stop(); }
};

void LifecycleAndRouting() {
    Fixture f;
    Check(f.mixer->Start(), "start");
    Check(f.probe->Wait(false), "idle timer must run without incoming frames");
    Check(f.input->Publish(Frame(3)) == 0, "reject unrelated endpoint");
    auto mismatch = Frame();
    mismatch.source.track_id = 11;
    Check(!f.mixer->SubmitFrame(mismatch), "reject mismatched track");
    Check(f.input->Publish(Frame()) == 1, "accept alice");
    Check(f.input->Publish(Frame(2)) == 1, "accept bob with same local track ID");
    f.mixer->Stop();
    f.mixer->Stop();
    Check(f.mixer->State() == MixState::Stopped, "stop state");
    Check(f.probe->starts == 1 && f.probe->inputs == 2 && f.probe->stops == 1, "drain and flush once");
    Check(f.probe->same_thread && f.probe->worker != std::this_thread::get_id(), "worker confinement");
    Check(f.sink->frames.size() == 3, "two outputs and one flush");
    for (const auto& event : f.sink->frames) {
        Check(event.source.session_id == "room-1" && event.source.stream_id == "composed" &&
              event.source.endpoint_id == 99 && event.source.track_id == 10, "output identity");
    }
    Check(f.input->Publish(Frame()) == 0, "unsubscribed after stop");
    Check(!f.mixer->Start(), "single-use task");
    const auto stats = f.mixer->Stats();
    Check(stats.accepted == 2 && stats.processed == 2 && stats.published == 3 &&
          stats.queue_frames == 0 && stats.queue_bytes == 0, "routing and queue stats");
}

void FailureHandling() {
    Fixture absent;
    auto missing = std::make_shared<ConferenceMixer>(Config(), absent.input, absent.output);
    Check(!missing->Start() && missing->State() == MixState::Failed &&
          !missing->LastError().empty(), "no backend must fail explicitly");
    auto feedback = std::make_shared<ConferenceMixer>(Config(), absent.input, absent.input,
                                                     std::make_unique<FakeBackend>(absent.probe));
    Check(!feedback->Start(), "reject feedback router");
    {
        Fixture f;
        f.probe->fail_start = true;
        Check(!f.mixer->Start() && f.probe->stops == 1, "clean partial initialization");
    }
    {
        Fixture f;
        f.probe->throw_input = true;
        Check(f.mixer->Start(), "exception test start");
        Check(f.input->Publish(Frame()) == 1, "exception test input");
        f.mixer->Stop();
        Check(f.mixer->State() == MixState::Failed && f.mixer->LastError() == "decode failure",
              "contain backend exception");
    }
    {
        Fixture f;
        f.sink->throw_on_flush = true;
        Check(f.mixer->Start(), "sink exception test start");
        Check(f.input->Publish(Frame()) == 1, "sink exception test input");
        f.mixer->Stop();
        Check(f.mixer->State() == MixState::Failed && f.mixer->Stats().output_rejected == 1,
              "contain sink exception during noexcept encoder flush");
    }
}

void QueueLimits() {
    for (bool byte_limit : {false, true}) {
        auto config = Config();
        if (byte_limit) config.max_queue_bytes = Frame().frame->StorageSize();
        else config.max_queue_frames = 1;
        Fixture f(config);
        f.probe->block = true;
        Check(f.mixer->Start(), "queue test start");
        Check(f.input->Publish(Frame()) == 1 && f.probe->Wait(true), "hold executing frame");
        Check(f.input->Publish(Frame(2)) == 1, "fill queue");
        Check(f.input->Publish(Frame()) == 0, "overflow rejected");
        f.probe->Release();
        f.mixer->Stop();
        const auto stats = f.mixer->Stats();
        Check(f.mixer->State() == MixState::Failed && stats.discarded == 1 &&
              stats.queue_frames == 0 && stats.queue_bytes == 0 && f.sink->frames.empty(),
              "overflow fails and clears queued frames without publishing");
    }
}
void ServiceFeatureSwitch()
{
    Fixture f;
    auto service = std::make_shared<ConferenceMixService>(f.input, f.output);
    auto status_reads = std::make_shared<int>(0);
    f.sink->on_frame = [weak = std::weak_ptr<ConferenceMixService>(service), status_reads] {
        if (auto owner = weak.lock()) {
            owner->Health();
            owner->LastError();
            ++*status_reads;
        }
    };
    server::ServerLauncher launcher;
    launcher.AddService("mix", service, false);
    Check(launcher.StartAll(), "disabled mix service must not prevent startup");
    Check(!service->StartConference(Config(), std::make_unique<FakeBackend>(f.probe)),
          "disabled service must refuse new conferences");
    Check(f.probe->starts == 0, "disabled service started backend");
    Check(launcher.SetEnabled("mix", true), "enable mix service");
    Check(!service->StartConference(Config(), {}), "still requires a real backend");
    Check(service->StartConference(Config(), std::make_unique<FakeBackend>(f.probe)), "start first room");
    auto other = std::make_shared<Probe>();
    auto config = Config();
    config.room_id = "room-2";
    config.output_endpoint_id = 100;
    Check(service->StartConference(config, std::make_unique<FakeBackend>(other)), "start second room");
    // Avoid concurrent writes to the test sink's vector; drain room one first.
    Check(service->StopConference("room-1"), "stop a single room");
    Check(f.probe->stops == 1 && service->State() == service::ServiceState::Running,
          "stopping one room must keep the global feature enabled");
    Check(service->StartConference(Config(), std::make_unique<FakeBackend>(f.probe)), "restart room with fresh task");
    Check(launcher.SetEnabled("mix", false), "disable all conference tasks");
    Check(f.probe->stops == 2 && other->stops == 1, "global disable must stop every task");
    Check(f.input->Publish(Frame()) == 0, "disabled service retained input subscriptions");
    Check(!service->StartConference(Config(), std::make_unique<FakeBackend>(f.probe)), "closed admission");
    Check(launcher.SetEnabled("mix", true), "reenable service");
    Check(f.input->Publish(Frame()) == 0, "reenable must not silently resume old rooms");
    Check(service->StartConference(Config(), std::make_unique<FakeBackend>(f.probe)), "new room after reenable");
    Check(f.input->Publish(Frame()) == 1, "reenabled room receives frames");
    launcher.StopAll();
    Check(f.probe->stops == 3 && f.sink->frames.size() == 2, "shutdown drains and flushes reenabled room");
    Check(*status_reads == 2, "status queries during flush must not deadlock shutdown");
}

void PerStreamMixPolicy()
{
    Fixture f;
    config::AppConfig initial;
    initial.conference_mix_enabled = true;
    auto settings = std::make_shared<config::ConfigStore>(initial);
    auto mix = std::make_shared<ConferenceMixService>(f.input, f.output, settings);
    Check(mix->Init() && mix->Start(), "configured service startup");
    Check(!mix->StartConference(Config(), std::make_unique<FakeBackend>(f.probe)), "missing stream identity must fail");
    auto first = Config();
    first.inputs = {{"alice", 1, 10, "session-a", "camera"}};
    auto second = Config();
    second.room_id = "other-room";
    second.output_endpoint_id = 100;
    second.inputs = {{"bob", 2, 10, "session-b", "camera"}};
    auto other = std::make_shared<Probe>();
    Check(mix->StartConference(first, std::make_unique<FakeBackend>(f.probe)), "first policy room");
    Check(mix->StartConference(second, std::make_unique<FakeBackend>(other)), "second policy room");
    auto frame = Frame();
    frame.source.session_id = "wrong-session";
    frame.source.stream_id = "camera";
    Check(f.input->Publish(frame) == 0, "endpoint ID alone must not bypass stream identity");
    settings->SetStreamConfig({"session-a", "camera"}, {std::nullopt, std::nullopt, false});
    mix->RefreshStreamPolicies();
    Check(f.probe->stops == 1 && !mix->StopConference(first.room_id), "disabled input must end its task");
    Check(!mix->StartConference(first, std::make_unique<FakeBackend>(f.probe)), "disabled stream task must not restart");
    frame = Frame(2);
    frame.source.session_id = "session-b";
    frame.source.stream_id = "camera";
    Check(f.input->Publish(frame) == 1, "unrelated room must keep receiving frames");
    mix->Stop();
    Check(other->inputs == 1 && other->stops == 1, "unrelated room lifecycle");
}
} // namespace

int main() {
    try {
        LifecycleAndRouting();
        FailureHandling();
        QueueLimits();
        ServiceFeatureSwitch();
        PerStreamMixPolicy();
        std::cout << "Conference mix framework tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
