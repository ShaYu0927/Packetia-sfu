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
    explicit BandwidthTransport(uint64_t id) : id_(id)
    {
        NetworkControllerConfig config;
        config.bitrate = {1000000, 1000000, 1000000};
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
