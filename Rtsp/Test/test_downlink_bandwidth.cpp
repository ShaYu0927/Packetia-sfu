#include <gtest/gtest.h>
#include "DownlinkTrackManager.h"
#include "SfuEndpoint.h"
#include "MediaStreamRegistry.h"
#include "MediaStreamAffinity.h"
#include "WorkerRegistry.h"
#include <chrono>
#include <future>
#include <thread>

namespace
{
using namespace media;
uint64_t Now()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
uint16_t U16(const uint8_t* p) { return (uint16_t(p[0]) << 8) | p[1]; }
void Put16(uint8_t* p, uint16_t value) { p[0] = uint8_t(value >> 8); p[1] = uint8_t(value); }

// Actual wire TWCC, with manually recorded sends to keep controller warmup
// deterministic. Constraints fix its capacity at 1 Mbit/s, not its validity.
void Feedback(const std::shared_ptr<SendSideController>& controller, uint16_t sequence, uint64_t at)
{
    for (unsigned i = 0; i < 10; ++i)
    {
        PacketSendInfo sent;
        sent.transport_sequence = sequence + i;
        sent.send_time_ms = at - 120 + i * 12;
        sent.size_bytes = 1250;
        controller->OnPacketSent(sent);
    }
    std::vector<uint8_t> feedback(32, 0);
    feedback[0] = 0x8f; feedback[1] = 205; feedback[3] = 7;
    Put16(feedback.data() + 12, sequence);
    Put16(feedback.data() + 14, 10);
    const uint32_t reference = uint32_t(at * 1000 / 64000) & 0xffffff;
    feedback[16] = uint8_t(reference >> 16); feedback[17] = uint8_t(reference >> 8); feedback[18] = uint8_t(reference);
    Put16(feedback.data() + 20, 0xbff0); // 10 received, small-delta statuses.
    for (size_t i = 22; i < 32; ++i) feedback[i] = 48;
    ASSERT_TRUE(controller->OnRtcpPacket(feedback.data(), feedback.size(), at));
}

class BandwidthTransport : public IMediaTransport
{
public:
    explicit BandwidthTransport(uint64_t id, uint64_t feedback_timeout_ms = 5000,
                                NetworkControllerConfig config = {}) : id_(id)
    {
        config.bitrate = {1000000, 1000000, 1000000};
        config.feedback_timeout_ms = feedback_timeout_ms;
        controller = std::make_shared<SendSideController>(config);
    }
    uint64_t Id() const noexcept override { return id_; }
    MediaTransportProtocol Protocol() const noexcept override { return MediaTransportProtocol::Udp; }
    MediaTransportState State() const noexcept override { return MediaTransportState::Connected; }
    SendResult Send(MediaPacketType, const uint8_t* data, size_t size, bool retry) override
    {
        packets.emplace_back(data, data + size);
        retries += retry;
        return SendResult::Ok;
    }
    void SetPacketSink(std::weak_ptr<IMediaPacketSink>) override {}
    void Close() override {}
    std::shared_ptr<SendSideController> GetSendSideController() override { return controller; }
    size_t MediaCount() const
    {
        size_t count = 0;
        for (const auto& packet : packets) if (!(packet[0] & 0x20)) ++count;
        return count;
    }
    uint64_t id_;
    size_t retries = 0;
    std::shared_ptr<SendSideController> controller;
    std::vector<std::vector<uint8_t>> packets;
};

std::vector<uint8_t> Packet(uint16_t sequence, uint32_t timestamp, uint8_t nal = 0x65)
{
    return {0x80, 0xe0, uint8_t(sequence >> 8), uint8_t(sequence),
        uint8_t(timestamp >> 24), uint8_t(timestamp >> 16), uint8_t(timestamp >> 8), uint8_t(timestamp),
        0, 0, 0, 77, nal, 0x80};
}

struct Downlinks
{
    std::shared_ptr<SfuEndpoint> source = std::make_shared<SfuEndpoint>(101);
    std::shared_ptr<SfuEndpoint> destination = std::make_shared<SfuEndpoint>(102);
    rtsp::RtcpDispatcher dispatcher;
    endpoint::DownlinkTrackManager manager;
    Downlinks() { manager.SetDispatcher(&dispatcher); }
    std::shared_ptr<endpoint::Subscription> Add(const std::string& id,
        const std::shared_ptr<BandwidthTransport>& transport, uint32_t ssrc, bool audio = false)
    {
        auto route = std::make_shared<endpoint::Subscription>();
        route->source = source; route->destination = destination;
        route->source_track_id = id;
        rtsp::RtpSenderTrackConfig config;
        config.local_ssrc = ssrc; config.transport_cc_extension_id = 3;
        config.sample_rate = audio ? 48000 : 90000;
        const auto codec = audio ? CodecId::OPUS : CodecId::H264;
        EXPECT_TRUE(manager.Configure(id, config, transport, codec));
        ::TrackInfo info;
        info.codec_id = codec; info.clock_rate = config.sample_rate;
        EXPECT_TRUE(manager.Subscribe(id, info, {}, route, {}));
        return route;
    }
    void Input(const std::shared_ptr<endpoint::Subscription>& route, const std::vector<uint8_t>& packet)
    { manager.Deliver(route, common::BufferView(packet.data(), packet.size())); }
};

struct ProbeCalls
{
    ProbeDecision decision{true, std::nullopt};
    std::vector<ProbeEvent> events;
    std::vector<PacketSendInfo> feedback;
    int resets = 0;
};

class InjectedProbes final : public IProbeController
{
public:
    explicit InjectedProbes(std::shared_ptr<ProbeCalls> calls) : calls_(std::move(calls)) {}
    ProbeDecision OnProcessInterval(const ProbeContext&) override { return calls_->decision; }
    void OnFeedback(const TransportFeedback& feedback, const PacketGroupObservation&, const BweResult&) override
    {
        for (const auto& packet : feedback.packet_feedbacks)
            if (packet.sent_packet.is_probe) calls_->feedback.push_back(packet.sent_packet);
    }
    void OnProbeEvent(const ProbeEvent& event) override { calls_->events.push_back(event); }
    void Reset() override { ++calls_->resets; calls_->decision = {true, std::nullopt}; }
private:
    std::shared_ptr<ProbeCalls> calls_;
};

NetworkControllerConfig ProbeConfig(const std::shared_ptr<ProbeCalls>& calls)
{
    NetworkControllerConfig config;
    config.probe_controller_factory = [calls] { return std::make_unique<InjectedProbes>(calls); };
    return config;
}

void PauseVideo(Downlinks& links, const std::shared_ptr<BandwidthTransport>& transport,
                const std::shared_ptr<endpoint::Subscription>& route, uint64_t now)
{
    DownlinkSubscriptionPolicy policy;
    policy.bitrate_hint_bps = 2000000;
    ASSERT_TRUE(links.manager.SetPolicy("video", policy));
    links.Input(route, Packet(100, 1000));
    Feedback(transport->controller, 10000, now - 2200);
    Feedback(transport->controller, 10010, now);
    links.manager.Tick(now, true);
    links.manager.Tick(now + 301, true);
    ASSERT_EQ(route->bandwidth_state.load(), endpoint::DownlinkBandwidthState::Paused);
}

TEST(DownlinkBandwidth, InjectedStrategyCanSuppressLegacyProbes)
{
    Downlinks links;
    auto calls = std::make_shared<ProbeCalls>();
    auto transport = std::make_shared<BandwidthTransport>(1, 5000, ProbeConfig(calls));
    auto route = links.Add("video", transport, 11);
    const auto now = Now();
    PauseVideo(links, transport, route, now);
    links.manager.Tick(now + 1000, true);
    EXPECT_EQ(transport->packets.size(), 1U);
    EXPECT_TRUE(calls->events.empty());
}

TEST(DownlinkBandwidth, CustomPlanIsBoundedAndWireFeedbackRetainsProbeIdentity)
{
    Downlinks links;
    auto calls = std::make_shared<ProbeCalls>();
    auto transport = std::make_shared<BandwidthTransport>(1, 5000, ProbeConfig(calls));
    auto route = links.Add("video", transport, 11);
    const auto now = Now();
    PauseVideo(links, transport, route, now);
    calls->decision = {true, ProbePlan{100000000, 10000, 1000000}};
    links.manager.Tick(now + 302, true);
    ASSERT_EQ(calls->events.size(), 1U);
    const auto plan = calls->events[0].plan;
    EXPECT_EQ(plan.bitrate_bps, 1000000U);
    EXPECT_EQ(plan.duration_ms, 500U);
    EXPECT_EQ(plan.max_bytes, 128U * 1024);
    ASSERT_GT(transport->packets.size(), 1U);
    const auto& padding = transport->packets.back();
    ASSERT_TRUE(padding[0] & 0x20);
    ASSERT_EQ(U16(padding.data() + 12), 0xbede);
    ASSERT_EQ(padding[16], 0x31); // TWCC extension id=3, two-byte value.
    const auto sequence = U16(padding.data() + 17);
    std::vector<uint8_t> feedback(24, 0);
    feedback[0] = 0x8f; feedback[1] = 205; feedback[3] = 5;
    Put16(feedback.data() + 12, sequence);
    Put16(feedback.data() + 14, 1);
    Put16(feedback.data() + 20, 0x2001); // One received packet, small delta.
    feedback[22] = 4;
    ASSERT_TRUE(transport->controller->OnRtcpPacket(feedback.data(), feedback.size(), now + 303));
    ASSERT_EQ(calls->feedback.size(), 1U);
    EXPECT_EQ(calls->feedback[0].probe_cluster_id, plan.cluster_id);
    EXPECT_EQ(calls->feedback[0].media_kind, MediaKind::Padding);
    EXPECT_EQ(calls->feedback[0].size_bytes, padding.size());

    calls->decision = {true, std::nullopt};
    links.manager.Tick(now + 502, true);
    links.manager.Tick(now + 802, true);
    ASSERT_EQ(calls->events.size(), 2U);
    EXPECT_EQ(calls->events.back().type, ProbeEventType::Finished);
    EXPECT_GT(calls->events.back().accounted_bytes, 0U);
    EXPECT_LE(calls->events.back().accounted_bytes, plan.max_bytes);
    calls->decision = {true, ProbePlan{100000, 500, 10000}};
    links.manager.Tick(now + 803, true);
    ASSERT_EQ(calls->events.size(), 3U);
    EXPECT_GT(calls->events.back().plan.cluster_id, plan.cluster_id);
    ASSERT_TRUE(links.manager.Unsubscribe("video"));
    ASSERT_EQ(calls->events.size(), 4U);
    EXPECT_EQ(calls->events.back().type, ProbeEventType::Aborted);
}

TEST(DownlinkBandwidth, TimeoutCancelsExecutionAndResetsOnlyItsTransportAlgorithms)
{
    Downlinks links;
    auto a = std::make_shared<ProbeCalls>();
    auto b = std::make_shared<ProbeCalls>();
    auto transport = std::make_shared<BandwidthTransport>(1, 5000, ProbeConfig(a));
    auto other = std::make_shared<BandwidthTransport>(2, 5000, ProbeConfig(b));
    auto route = links.Add("video", transport, 11);
    links.Add("other", other, 21);
    const auto now = Now();
    PauseVideo(links, transport, route, now);
    // Start just before expiration so the timeout interrupts an active cluster.
    a->decision = {true, ProbePlan{100000, 500, 10000}};
    links.manager.Tick(now + 4900, true);
    ASSERT_EQ(a->events.size(), 1U);
    const auto count = transport->packets.size();
    links.manager.Tick(now + 5000, true);
    EXPECT_EQ(transport->packets.size(), count);
    EXPECT_EQ(a->resets, 1);
    EXPECT_EQ(b->resets, 0);
    EXPECT_EQ(a->events.size(), 1U); // Reset invalidates events from the old generation.
    EXPECT_EQ(route->bandwidth_state.load(), endpoint::DownlinkBandwidthState::Paused);
}

TEST(DownlinkBandwidth, IsolatesTransportsBlocksMediaAndRetriesButAllowsBoundedProbes)
{
    Downlinks links;
    auto b = std::make_shared<BandwidthTransport>(1);
    auto c = std::make_shared<BandwidthTransport>(2);
    auto b1 = links.Add("b1", b, 11);
    auto b2 = links.Add("b2", b, 12);
    auto c1 = links.Add("c1", c, 21);
    DownlinkSubscriptionPolicy policy;
    policy.bitrate_hint_bps = 600000;
    policy.priority = 200;
    ASSERT_TRUE(links.manager.SetPolicy("b1", policy));
    policy.priority = 1;
    ASSERT_TRUE(links.manager.SetPolicy("b2", policy));
    links.Input(b1, Packet(100, 1000));
    links.Input(b2, Packet(100, 1000));
    links.Input(c1, Packet(100, 1000));
    const auto now = Now();
    Feedback(b->controller, 10000, now - 2200);
    Feedback(b->controller, 10010, now);
    ASSERT_TRUE(b->controller->GetBandwidthBudget().estimate_valid);
    links.manager.Tick(now, true);
    links.manager.Tick(now + 301, true);
    EXPECT_EQ(b1->bandwidth_state.load(), endpoint::DownlinkBandwidthState::Forwarding);
    EXPECT_EQ(b2->bandwidth_state.load(), endpoint::DownlinkBandwidthState::Paused);
    EXPECT_EQ(c1->bandwidth_state.load(), endpoint::DownlinkBandwidthState::Forwarding);
    const auto count = b->MediaCount();
    links.Input(b2, Packet(500, 10000, 0x61));
    links.dispatcher.OnNack(1, 12, std::vector<uint16_t>{100}.data(), 1);
    EXPECT_EQ(b->MediaCount(), count);
    EXPECT_EQ(b->retries, 0U);
    ASSERT_GT(b->packets.size(), count); // Only padded RTP passed the probe gate.
    size_t bytes = 0;
    for (const auto& packet : b->packets) if (packet[0] & 0x20) bytes += packet.size();
    EXPECT_LE(bytes, 128U * 1024);
    links.Input(c1, Packet(101, 2000));
    EXPECT_EQ(c->MediaCount(), 2U);
}

TEST(DownlinkBandwidth, PolicyRecoveryWaitsForKeyframeAndPreservesSequenceContinuity)
{
    Downlinks links;
    auto transport = std::make_shared<BandwidthTransport>(1);
    auto route = links.Add("video", transport, 11);
    DownlinkSubscriptionPolicy policy;
    policy.bitrate_hint_bps = 2000000;
    ASSERT_TRUE(links.manager.SetPolicy("video", policy));
    links.Input(route, Packet(100, 1000));
    const auto now = Now();
    Feedback(transport->controller, 10000, now - 2200);
    Feedback(transport->controller, 10010, now);
    links.manager.Tick(now, true);
    links.manager.Tick(now + 301, true);
    ASSERT_EQ(route->bandwidth_state.load(), endpoint::DownlinkBandwidthState::Paused);
    // Exempt this track from allocation without touching consumer pause state.
    policy.enabled = false;
    ASSERT_TRUE(links.manager.SetPolicy("video", policy));
    ASSERT_EQ(route->bandwidth_state.load(), endpoint::DownlinkBandwidthState::WaitingKeyFrame);
    const auto count = transport->MediaCount();
    links.Input(route, Packet(1000, 90000, 0x61));
    EXPECT_EQ(transport->MediaCount(), count);
    const auto last_sequence = U16(transport->packets.back().data() + 2);
    links.Input(route, Packet(1001, 93000));
    EXPECT_EQ(route->bandwidth_state.load(), endpoint::DownlinkBandwidthState::Forwarding);
    EXPECT_EQ(transport->MediaCount(), count + 1);
    EXPECT_EQ(U16(transport->packets.back().data() + 2), uint16_t(last_sequence + 1));
    links.Input(route, Packet(999, 89999));
    EXPECT_EQ(transport->MediaCount(), count + 1);
}

TEST(DownlinkBandwidth, LastUnsubscribeRemovesOnlyItsObserverAndOldRouteCannotResume)
{
    Downlinks links;
    int notifications = 0, app = 0;
    links.manager.SetSchedule([&] { ++notifications; });
    auto transport = std::make_shared<BandwidthTransport>(1);
    transport->controller->SetUpdateCallback([&](const auto&) { ++app; });
    auto route = links.Add("video", transport, 11);
    transport->controller->OnNetworkRouteChanged();
    EXPECT_EQ(notifications, 1);
    EXPECT_EQ(app, 1);
    ASSERT_TRUE(links.manager.Unsubscribe("video"));
    const auto before = notifications;
    transport->controller->OnNetworkRouteChanged();
    EXPECT_EQ(notifications, before);
    EXPECT_EQ(app, 2);
    EXPECT_TRUE(route->Closed());
    links.Input(route, Packet(100, 1000));
    EXPECT_TRUE(transport->packets.empty());
}

TEST(DownlinkBandwidth, UserAndSourcePausesSurviveAllocationChangesAndSuppressProbes)
{
    Downlinks links;
    auto transport = std::make_shared<BandwidthTransport>(1);
    auto route = links.Add("video", transport, 11);
    MediaStreamRegistry registry;
    auto stream = std::make_shared<MediaStream>("bandwidth-user-pause");
    ASSERT_TRUE(stream->ReplaceTracks({{0, MediaType::Video, CodecType::H264}}));
    ASSERT_TRUE(stream->BindTrackEndpoint(0, 101));
    ASSERT_TRUE(stream->StartPublishing());
    ASSERT_TRUE(registry.Register(stream));
    const auto track = stream->GetTracks()[0].ref;
    route->consumer = registry.AttachConsumer(track, "video");
    ASSERT_TRUE(route->consumer);
    DownlinkSubscriptionPolicy policy;
    policy.bitrate_hint_bps = 2000000;
    ASSERT_TRUE(links.manager.SetPolicy("video", policy));
    links.Input(route, Packet(100, 1000));
    const auto now = Now();
    Feedback(transport->controller, 10000, now - 2200);
    Feedback(transport->controller, 10010, now);
    links.manager.Tick(now, true);
    links.manager.Tick(now + 301, true);
    ASSERT_EQ(route->bandwidth_state.load(), endpoint::DownlinkBandwidthState::Paused);
    ASSERT_TRUE(registry.SetConsumerPaused(route->consumer->Id(), true));
    const auto count = transport->packets.size();
    links.manager.Tick(now + 5000, true);
    EXPECT_EQ(transport->packets.size(), count);
    policy.enabled = false;
    ASSERT_TRUE(links.manager.SetPolicy("video", policy));
    EXPECT_TRUE(route->consumer->Paused());
    links.Input(route, Packet(1000, 90000));
    EXPECT_EQ(transport->packets.size(), count);
    ASSERT_TRUE(registry.SetConsumerPaused(route->consumer->Id(), false));
    ASSERT_TRUE(registry.SetTrackPaused(track, true));
    links.Input(route, Packet(1001, 93000));
    EXPECT_EQ(transport->packets.size(), count);
    ASSERT_TRUE(registry.SetTrackPaused(track, false));
    links.Input(route, Packet(1002, 96000));
    EXPECT_EQ(transport->packets.size(), count + 1);
}

TEST(DownlinkBandwidth, FreshBudgetAutomaticallyRecoversAfterHeadroomRemainsStable)
{
    Downlinks links;
    auto transport = std::make_shared<BandwidthTransport>(1);
    auto route = links.Add("video", transport, 11);
    DownlinkSubscriptionPolicy policy;
    policy.bitrate_hint_bps = 2000000;
    ASSERT_TRUE(links.manager.SetPolicy("video", policy));
    links.Input(route, Packet(100, 1000));
    const auto now = Now();
    Feedback(transport->controller, 10000, now - 2200);
    Feedback(transport->controller, 10010, now);
    links.manager.Tick(now, true);
    links.manager.Tick(now + 301, true);
    ASSERT_EQ(route->bandwidth_state.load(), endpoint::DownlinkBandwidthState::Paused);
    // The source requirement falls, leaving room in the measured budget.
    policy.bitrate_hint_bps = 500000;
    ASSERT_TRUE(links.manager.SetPolicy("video", policy));
    EXPECT_EQ(route->bandwidth_state.load(), endpoint::DownlinkBandwidthState::Paused);
    links.manager.Tick(now + 1300, true);
    EXPECT_EQ(route->bandwidth_state.load(), endpoint::DownlinkBandwidthState::WaitingKeyFrame);
    links.Input(route, Packet(1000, 90000));
    EXPECT_EQ(route->bandwidth_state.load(), endpoint::DownlinkBandwidthState::Forwarding);
}

TEST(DownlinkBandwidth, PathChangesInvalidateBudgetAndRequireANewKeyframe)
{
    Downlinks links;
    auto transport = std::make_shared<BandwidthTransport>(1);
    auto route = links.Add("video", transport, 11);
    links.Input(route, Packet(100, 1000));
    const auto now = Now();
    Feedback(transport->controller, 10000, now - 2200);
    Feedback(transport->controller, 10010, now);
    const auto measured_at = transport->controller->GetBandwidthBudget().measured_at_ms;
    WeakNetFeedback rtt;
    rtt.now_ms = now + 100; rtt.rtt_ms = 50;
    transport->controller->OnReceiverFeedback(rtt);
    EXPECT_EQ(transport->controller->GetBandwidthBudget().measured_at_ms, measured_at);
    transport->controller->OnNetworkRouteChanged();
    EXPECT_FALSE(transport->controller->GetBandwidthBudget().estimate_valid);
    links.manager.Tick(now + 101, true);
    EXPECT_EQ(route->bandwidth_state.load(), endpoint::DownlinkBandwidthState::WaitingKeyFrame);
    links.Input(route, Packet(1000, 90000, 0x61));
    EXPECT_EQ(transport->MediaCount(), 1U);
    links.Input(route, Packet(1001, 93000));
    EXPECT_EQ(transport->MediaCount(), 2U);
    EXPECT_EQ(U16(transport->packets.back().data() + 2), 101);
    links.dispatcher.OnNack(1, 11, std::vector<uint16_t>{100}.data(), 1);
    EXPECT_EQ(transport->MediaCount(), 2U);
}

TEST(DownlinkBandwidth, IdleTickExpiresOnlyItsTransportAndKeepsExistingSubscriptionStates)
{
    Downlinks links;
    auto idle = std::make_shared<BandwidthTransport>(1);
    auto fresh = std::make_shared<BandwidthTransport>(2);
    auto active_video = links.Add("active", idle, 11);
    auto paused_video = links.Add("paused", idle, 12);
    auto other_video = links.Add("other", fresh, 21);
    DownlinkSubscriptionPolicy policy;
    policy.bitrate_hint_bps = 2000000;
    ASSERT_TRUE(links.manager.SetPolicy("paused", policy));
    const auto now = Now();
    Feedback(idle->controller, 10000, now - 2200);
    Feedback(idle->controller, 10010, now);
    Feedback(fresh->controller, 20000, now - 2200);
    Feedback(fresh->controller, 20010, now);
    links.manager.Tick(now, true);
    links.manager.Tick(now + 301, true);
    ASSERT_EQ(paused_video->bandwidth_state.load(), endpoint::DownlinkBandwidthState::Paused);
    const auto epoch = idle->controller->GetBandwidthBudget().path_epoch;
    Feedback(fresh->controller, 20020, now + 3500);
    Feedback(fresh->controller, 20030, now + 5000);
    links.manager.Tick(now + 5000, true); // No media arrived on the idle transport.
    EXPECT_FALSE(idle->controller->GetNetworkState().HasUpdates());
    EXPECT_FALSE(idle->controller->GetBandwidthBudget().estimate_valid);
    EXPECT_TRUE(idle->controller->GetBandwidthBudget().network_available);
    EXPECT_EQ(idle->controller->GetBandwidthBudget().path_epoch, epoch);
    EXPECT_TRUE(fresh->controller->GetBandwidthBudget().estimate_valid);
    EXPECT_EQ(active_video->bandwidth_state.load(), endpoint::DownlinkBandwidthState::Forwarding);
    EXPECT_EQ(paused_video->bandwidth_state.load(), endpoint::DownlinkBandwidthState::Paused);
    EXPECT_EQ(other_video->bandwidth_state.load(), endpoint::DownlinkBandwidthState::Forwarding);
    EXPECT_EQ(links.manager.Count(), 3U);
}

TEST(DownlinkBandwidth, TimeoutCallbackCanUnsubscribeWhileTheManagerTicks)
{
    Downlinks links;
    auto transport = std::make_shared<BandwidthTransport>(1);
    auto route = links.Add("video", transport, 11);
    const auto now = Now();
    WeakNetFeedback rr;
    rr.now_ms = now;
    transport->controller->OnReceiverFeedback(rr);
    transport->controller->SetUpdateCallback([&](const auto& update) {
        if (!update.HasUpdates()) EXPECT_TRUE(links.manager.Unsubscribe("video"));
    });
    links.manager.Tick(now + 5000, true);
    EXPECT_EQ(links.manager.Count(), 0U);
    EXPECT_TRUE(route->Closed());
}

class ConcurrentBandwidthTransport final : public BandwidthTransport
{
public:
    using BandwidthTransport::BandwidthTransport;
    SendResult Send(MediaPacketType type, const uint8_t* data, size_t size, bool retry) override
    {
        std::lock_guard<std::mutex> guard(mutex);
        return BandwidthTransport::Send(type, data, size, retry);
    }
    size_t SentMedia()
    {
        std::lock_guard<std::mutex> guard(mutex);
        return MediaCount();
    }
    std::mutex mutex;
};

TEST(DownlinkBandwidth, WorkerRecoveryTickExpiresFeedbackWithoutIncomingPackets)
{
    WorkerRegistry workers;
    workers.Add({POOL_MEDIA, 2, 128, ShardedWorkerPool::DropPolicy::DropTail},
        std::make_shared<utils::EndpointJobHandler>(&utils::EndpointManager::Instance()));
    ASSERT_EQ(workers.Start(), 0);
    auto source = std::make_shared<SfuEndpoint>(401);
    auto viewer = std::make_shared<SfuEndpoint>(402);
    auto transport = std::make_shared<BandwidthTransport>(1, 50);
    ::TrackInfo info;
    info.track_index = 0; info.type = TrackVideo; info.codec_id = CodecId::H264;
    info.codec_name = "H264"; info.clock_rate = 90000; info.payload_type = 96; info.ssrc = 77;
    ASSERT_TRUE(source->AddPublishedTrack("camera", std::make_shared<RtpTrackDescription>(info)));
    ASSERT_TRUE(source->Start());
    ASSERT_TRUE(viewer->Start());
    rtsp::RtpSenderTrackConfig config;
    config.local_ssrc = 11; config.transport_cc_extension_id = 3;
    ASSERT_TRUE(viewer->ConfigureSubscription("video", config, transport, CodecId::H264));
    ASSERT_TRUE(viewer->Subscribe("video", source, "camera"));
    // Set up state before registering the observer callback; no packet or real
    // sleep is needed to make the existing recovery worker observe expiration.
    auto done = std::make_shared<std::promise<void>>();
    auto result = done->get_future();
    std::atomic<int> clears{0};
    ASSERT_EQ(media_affinity::PostToMediaStream(viewer->Id(), 0, [&, done] {
        WeakNetFeedback rr;
        rr.now_ms = Now() - 100;
        transport->controller->OnReceiverFeedback(rr);
        transport->controller->SetUpdateCallback([&](const auto& update) {
            if (!update.HasUpdates()) ++clears;
        });
        EXPECT_TRUE(transport->controller->GetNetworkState().HasUpdates());
        done->set_value();
    }), 0);
    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ASSERT_TRUE(viewer->ScheduleRecoveryTick());
    auto drained = std::make_shared<std::promise<void>>();
    auto drained_result = drained->get_future();
    ASSERT_EQ(media_affinity::PostToMediaStream(viewer->Id(), 0, [drained] { drained->set_value(); }), 0);
    ASSERT_EQ(drained_result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(clears.load(), 1);
    EXPECT_FALSE(transport->controller->GetNetworkState().HasUpdates());
    const auto status = viewer->GetSubscriptionBandwidthStatus("video");
    ASSERT_TRUE(status);
    EXPECT_EQ(status->state, endpoint::DownlinkBandwidthState::Forwarding);
    viewer->Stop(); source->Stop();
    transport->controller->SetUpdateCallback({});
    workers.Stop();
}

TEST(DownlinkBandwidth, WireFeedbackSchedulesEndpointAllocationAndKeepsOtherSubscribersFlowing)
{
    WorkerRegistry workers;
    workers.Add({POOL_MEDIA, 4, 128, ShardedWorkerPool::DropPolicy::DropTail},
        std::make_shared<utils::EndpointJobHandler>(&utils::EndpointManager::Instance()));
    ASSERT_EQ(workers.Start(), 0);
    auto source = std::make_shared<SfuEndpoint>(301);
    auto b = std::make_shared<SfuEndpoint>(302);
    auto c = std::make_shared<SfuEndpoint>(303);
    auto bt = std::make_shared<ConcurrentBandwidthTransport>(1);
    auto ct = std::make_shared<ConcurrentBandwidthTransport>(2);
    ::TrackInfo info;
    info.track_index = 0; info.type = TrackVideo; info.codec_id = CodecId::H264;
    info.codec_name = "H264"; info.clock_rate = 90000; info.payload_type = 96; info.ssrc = 77;
    ASSERT_TRUE(source->AddPublishedTrack("camera", std::make_shared<RtpTrackDescription>(info)));
    ASSERT_TRUE(source->Start()); ASSERT_TRUE(b->Start()); ASSERT_TRUE(c->Start());
    rtsp::RtpSenderTrackConfig config;
    config.local_ssrc = 11; config.transport_cc_extension_id = 3;
    ASSERT_TRUE(b->ConfigureSubscription("video", config, bt, CodecId::H264));
    ASSERT_TRUE(b->Subscribe("video", source, "camera"));
    config.local_ssrc = 21;
    ASSERT_TRUE(c->ConfigureSubscription("video", config, ct, CodecId::H264));
    ASSERT_TRUE(c->Subscribe("video", source, "camera"));
    DownlinkSubscriptionPolicy policy;
    policy.bitrate_hint_bps = 2000000;
    ASSERT_TRUE(b->SetSubscriptionBandwidthPolicy("video", policy));
    const auto drain = [&](uint64_t endpoint, uint32_t ssrc) {
        auto done = std::make_shared<std::promise<void>>();
        auto future = done->get_future();
        ASSERT_EQ(media_affinity::PostToMediaStream(endpoint, ssrc, [done] { done->set_value(); }), 0);
        ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    };
    const auto input = [&](const std::vector<uint8_t>& packet) {
        WorkJob job;
        job.payload = common::SharedBuffer::TryCopy(packet.data(), packet.size());
        source->OnRtp(job);
        drain(302, 11); drain(303, 21);
    };
    input(Packet(100, 1000));
    EXPECT_EQ(bt->SentMedia(), 1U); EXPECT_EQ(ct->SentMedia(), 1U);
    const auto now = Now();
    Feedback(bt->controller, 10000, now - 2200);
    Feedback(bt->controller, 10010, now);
    drain(302, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(320));
    ASSERT_TRUE(b->ScheduleRecoveryTick());
    drain(302, 0);
    const auto status = b->GetSubscriptionBandwidthStatus("video");
    ASSERT_TRUE(status);
    EXPECT_EQ(status->state, endpoint::DownlinkBandwidthState::Paused);
    input(Packet(500, 90000, 0x61));
    EXPECT_EQ(bt->SentMedia(), 1U);
    EXPECT_EQ(ct->SentMedia(), 2U);
    policy.enabled = false;
    ASSERT_TRUE(b->SetSubscriptionBandwidthPolicy("video", policy));
    input(Packet(501, 93000));
    EXPECT_EQ(bt->SentMedia(), 2U);
    EXPECT_EQ(ct->SentMedia(), 3U);
    b->Stop(); c->Stop(); source->Stop();
    workers.Stop();
}
}
