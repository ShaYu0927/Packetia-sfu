#include <gtest/gtest.h>
#include "MediaEndpoint.h"
#include "Room.h"
#include "SdpTrackBinding.h"
#include "MediaStreamAffinity.h"
#include "WorkerRegistry.h"
#include "RtcpNack.h"
#include "RtcpFeedback.h"
#include <future>

namespace
{
class CaptureTransport : public IMediaTransport
{
public:
    uint64_t Id() const noexcept override { return 100; }
    MediaTransportProtocol Protocol() const noexcept override { return MediaTransportProtocol::Udp; }
    MediaTransportState State() const noexcept override { return MediaTransportState::Connected; }
    SendResult Send(MediaPacketType, const uint8_t* data, size_t size, bool = false) override {
        std::lock_guard<std::mutex> lock(mutex);
        packets.emplace_back(data, data + size);
        return SendResult::Ok;
    }
    void SetPacketSink(std::weak_ptr<IMediaPacketSink>) override {}
    void Close() override {}
    size_t Count() { std::lock_guard<std::mutex> lock(mutex); return packets.size(); }
    std::vector<uint8_t> Last() { std::lock_guard<std::mutex> lock(mutex); return packets.back(); }
    std::mutex mutex;
    std::vector<std::vector<uint8_t>> packets;
};

std::shared_ptr<RtpTrackDescription> Description(int index, bool audio, uint8_t pt)
{
    ::TrackInfo info;
    info.track_index = index;
    info.type = audio ? TrackAudio : TrackVideo;
    info.codec_id = audio ? CodecId::OPUS : CodecId::H264;
    info.codec_name = audio ? "opus" : "H264";
    info.payload_type = pt;
    info.clock_rate = audio ? 48000 : 90000;
    info.channels = audio ? 2 : 0;
    return std::make_shared<RtpTrackDescription>(info);
}

sdp::SdpMedia Negotiated(const std::string& mid, bool audio, uint8_t pt, uint32_t ssrc = 0)
{
    sdp::SdpMedia result;
    result.mid = mid;
    result.media = audio ? "audio" : "video";
    result.port = 9;
    result.proto = "UDP/TLS/RTP/SAVPF";
    result.direction = sdp::MediaDirection::RecvOnly;
    result.rtcpMux = true;
    result.codecs.push_back({pt, audio ? "opus" : "H264", audio ? 48000 : 90000,
        audio ? 2 : 0, "", {}});
    result.headerExtensions.push_back({1, "urn:ietf:params:rtp-hdrext:sdes:mid"});
    if (ssrc) result.ssrcs.push_back({ssrc, {}});
    return result;
}

std::vector<uint8_t> Packet(uint32_t ssrc, uint8_t pt, uint16_t seq = 1, const std::string& mid = {})
{
    std::vector<uint8_t> p{0x80, static_cast<uint8_t>(0x80 | pt), static_cast<uint8_t>(seq >> 8),
        static_cast<uint8_t>(seq), 0, 0, 0, 1, static_cast<uint8_t>(ssrc >> 24),
        static_cast<uint8_t>(ssrc >> 16), static_cast<uint8_t>(ssrc >> 8), static_cast<uint8_t>(ssrc)};
    if (!mid.empty()) {
        p[0] |= 0x10;
        p.insert(p.end(), {0xBE, 0xDE, 0, 1, static_cast<uint8_t>(0x10 | (mid.size() - 1))});
        p.insert(p.end(), mid.begin(), mid.end());
        while (p.size() % 4) p.push_back(0);
    }
    p.insert(p.end(), {0x65, 0xAA, 0xBB});
    return p;
}

void Input(const std::shared_ptr<media::SfuEndpoint>& endpoint, const std::vector<uint8_t>& bytes,
           const std::string& hint = {}, bool rtcp = false)
{
    WorkJob job;
    job.payload = common::SharedBuffer::TryCopy(bytes.data(), bytes.size());
    job.media_track_id = hint;
    if (rtcp) endpoint->OnRtcp(job); else endpoint->OnRtp(job);
}

class SfuConference : public ::testing::Test
{
protected:
    void SetUp() override {
        registry.Add({POOL_MEDIA, 4, 128, ShardedWorkerPool::DropPolicy::DropTail},
            std::make_shared<utils::EndpointJobHandler>(&utils::EndpointManager::Instance()));
        ASSERT_EQ(registry.Start(), 0);
    }
    void TearDown() override { registry.Stop(); }
    void Drain(uint64_t key) {
        auto done = std::make_shared<std::promise<void>>();
        auto future = done->get_future();
        ASSERT_EQ(WorkerService::post_fn(POOL_MEDIA, key, [done] { done->set_value(); }), 0);
        ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    }
    void Configure(const std::shared_ptr<media::SfuEndpoint>& endpoint, const std::string& id,
                   const std::shared_ptr<CaptureTransport>& transport, bool audio, uint32_t ssrc, uint8_t pt) {
        rtsp::RtpSenderTrackConfig config;
        config.local_ssrc = ssrc;
        config.payload_type = pt;
        config.sample_rate = audio ? 48000 : 90000;
        ASSERT_TRUE(endpoint->ConfigureSubscription(id, config, transport, audio ? CodecId::OPUS : CodecId::H264));
    }
    WorkerRegistry registry;
};

TEST_F(SfuConference, SharedPayloadNeedsMidOrExplicitBindingAndCannotChangeTracks)
{
    auto endpoint = std::make_shared<media::SfuEndpoint>(1001);
    ASSERT_TRUE(endpoint->AddPublishedTrack("audio", Description(0, true, 96), "a", 1));
    ASSERT_TRUE(endpoint->AddPublishedTrack("video", Description(1, false, 96), "v", 1));
    ASSERT_TRUE(endpoint->Start());
    std::vector<media::TrackId> frames;
    endpoint->AddEncodedFrameCallback([&](const auto& frame) { frames.push_back(frame->info.track_id); });
    Input(endpoint, Packet(11, 96)); // Ambiguous PT must not claim an SSRC.
    EXPECT_TRUE(frames.empty());
    Input(endpoint, Packet(11, 96, 1, "v"));
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames.back(), 1U);
    Input(endpoint, Packet(11, 96, 2, "a")); // Existing binding cannot move.
    EXPECT_EQ(frames.size(), 1U);
    Input(endpoint, Packet(22, 96, 1), "audio");
    ASSERT_EQ(frames.size(), 2U);
    EXPECT_EQ(frames.back(), 0U);
    ASSERT_TRUE(endpoint->RemovePublishedTrack("video"));
    Input(endpoint, Packet(11, 96, 3)); // A late video packet must not become audio.
    EXPECT_EQ(frames.size(), 2U);
    Input(endpoint, Packet(22, 96, 2), "audio");
    EXPECT_EQ(frames.size(), 3U);
    endpoint->Stop();
    Input(endpoint, Packet(22, 96, 3), "audio");
    EXPECT_EQ(frames.size(), 3U);
}

TEST_F(SfuConference, ReplacePublishedTracksPausesAndResumesMidWithOriginalIndexAndRtcpCallback)
{
    auto endpoint = std::make_shared<media::SfuEndpoint>(1101);
    ASSERT_TRUE(endpoint->AddPublishedTrack("camera", Description(7, false, 102), "v", 1));
    ASSERT_TRUE(endpoint->Start());
    std::vector<media::TrackId> frames;
    endpoint->AddEncodedFrameCallback([&](const auto& frame) { frames.push_back(frame->info.track_id); });
    size_t feedback = 0;
    endpoint->SetTrackRtcpSendCallback("camera", [&](const uint8_t*, size_t) { ++feedback; return true; });
    Input(endpoint, Packet(11, 102, 1, "v"));
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames.back(), 7U);
    ASSERT_TRUE(endpoint->ReplacePublishedTracks({}));
    EXPECT_EQ(endpoint->PublishedTrackCount(), 0U);
    Input(endpoint, Packet(11, 102, 2, "v"));
    Input(endpoint, Packet(11, 102, 2));
    EXPECT_EQ(frames.size(), 1U);

    // No a=ssrc is required to resume a previously learned source on this MID.
    ASSERT_TRUE(endpoint->ReplacePublishedTracks({Negotiated("v", false, 102)}));
    ::TrackInfo info;
    ASSERT_TRUE(endpoint->GetPublishedTrack("camera", info));
    EXPECT_EQ(info.track_index, 7);
    Input(endpoint, Packet(11, 102, 1, "v")); // Recreated receiver accepts a reset sequence.
    ASSERT_EQ(frames.size(), 2U);
    EXPECT_EQ(frames.back(), 7U);
    endpoint->OnTrackPli(11);
    EXPECT_EQ(feedback, 1U);
}

TEST_F(SfuConference, ReplacePublishedTracksChangesPayloadAndAddsTrackWithoutResettingOtherReceiversOrRoutes)
{
    auto source = std::make_shared<media::SfuEndpoint>(1102);
    auto destination = std::make_shared<media::SfuEndpoint>(1103);
    auto audio = Negotiated("a", true, 111, 22);
    auto video = Negotiated("v", false, 102, 11);
    ASSERT_TRUE(source->ReplacePublishedTracks({audio, video}));
    ASSERT_TRUE(source->Start());
    ASSERT_TRUE(destination->Start());
    ::TrackInfo oldAudio, oldVideo;
    ASSERT_TRUE(source->GetPublishedTrack("a", oldAudio));
    ASSERT_TRUE(source->GetPublishedTrack("v", oldVideo));
    auto transport = std::make_shared<CaptureTransport>();
    Configure(destination, "audio", transport, true, 778, 109);
    Configure(destination, "video", transport, false, 777, 96);
    ASSERT_TRUE(destination->Subscribe("audio", source, "a"));
    ASSERT_TRUE(destination->Subscribe("video", source, "v"));
    size_t audioFrames = 0, videoFrames = 0;
    source->AddEncodedFrameCallback([&](const auto& frame)
    {
        if (frame->info.track_id == static_cast<media::TrackId>(oldAudio.track_index)) ++audioFrames;
        if (frame->info.track_id == static_cast<media::TrackId>(oldVideo.track_index)) ++videoFrames;
    });
    Input(source, Packet(22, 111));
    Input(source, Packet(11, 102));
    Drain(media_affinity::MakeStreamKey(destination->Id(), 777));
    Drain(media_affinity::MakeStreamKey(destination->Id(), 778));
    ASSERT_EQ(transport->Count(), 2U);
    ASSERT_EQ(audioFrames, 1U);
    ASSERT_EQ(videoFrames, 1U);

    video.codecs.front().payloadType = 103;
    ASSERT_TRUE(source->ReplacePublishedTracks({video, audio, Negotiated("screen", false, 104, 33)}));
    ::TrackInfo current;
    ASSERT_TRUE(source->GetPublishedTrack("a", current));
    EXPECT_EQ(current.track_index, oldAudio.track_index);
    ASSERT_TRUE(source->GetPublishedTrack("v", current));
    EXPECT_EQ(current.track_index, oldVideo.track_index);
    ASSERT_TRUE(source->GetPublishedTrack("screen", current));
    EXPECT_NE(current.track_index, oldVideo.track_index);
    EXPECT_NE(current.track_index, oldAudio.track_index);
    EXPECT_EQ(destination->SubscriptionCount(), 2U);
    Input(source, Packet(22, 111)); // Duplicate: unchanged audio receiver must retain sequence history.
    EXPECT_EQ(audioFrames, 1U);
    Input(source, Packet(11, 102, 2));
    EXPECT_EQ(videoFrames, 1U); // Old video PT cannot reach the replaced receiver.
    Input(source, Packet(11, 103, 2));
    EXPECT_EQ(videoFrames, 2U); // The new PT really reaches the updated receiver.
    Input(source, Packet(22, 111, 2));
    EXPECT_EQ(audioFrames, 2U);
    Drain(media_affinity::MakeStreamKey(destination->Id(), 777));
    Drain(media_affinity::MakeStreamKey(destination->Id(), 778));
    EXPECT_GE(transport->Count(), 4U);
}

TEST_F(SfuConference, ReplacePublishedTracksRejectsInvalidSetsWithoutChangingOldBindings)
{
    auto source = std::make_shared<media::SfuEndpoint>(1104);
    auto audio = Negotiated("a", true, 111, 22);
    auto video = Negotiated("v", false, 102, 11);
    ASSERT_TRUE(source->ReplacePublishedTracks({audio, video}));
    ASSERT_TRUE(source->Start());
    size_t frames = 0;
    source->AddEncodedFrameCallback([&](const auto&) { ++frames; });
    Input(source, Packet(22, 111));
    ASSERT_EQ(frames, 1U);
    auto changedAudio = audio;
    changedAudio.codecs.front().payloadType = 112;
    auto invalid = video;
    invalid.mid = "a";
    EXPECT_FALSE(source->ReplacePublishedTracks({changedAudio, invalid}));
    invalid = video;
    invalid.ssrcs.front().ssrc = 22;
    EXPECT_FALSE(source->ReplacePublishedTracks({changedAudio, invalid}));
    invalid = video;
    invalid.codecs.front().encodingName = "H265";
    EXPECT_FALSE(source->ReplacePublishedTracks({changedAudio, invalid}));
    invalid = video;
    invalid.codecs.front().payloadType = 128;
    EXPECT_FALSE(source->ReplacePublishedTracks({changedAudio, invalid}));
    invalid = video;
    invalid.ssrcGroups.push_back({"FID", {11, 12}});
    EXPECT_FALSE(source->ReplacePublishedTracks({changedAudio, invalid}));
    ::TrackInfo current;
    ASSERT_TRUE(source->GetPublishedTrack("a", current));
    EXPECT_EQ(current.payload_type, 111);
    EXPECT_EQ(source->PublishedTrackCount(), 2U);
    Input(source, Packet(22, 112, 2));
    EXPECT_EQ(frames, 1U);
    Input(source, Packet(22, 111, 2));
    EXPECT_EQ(frames, 2U);
}

TEST_F(SfuConference, ReplacePublishedTracksRejectsSubscribedEncodingChangesAndOnlyDetachesPausedTrack)
{
    auto source = std::make_shared<media::SfuEndpoint>(1105);
    auto destination = std::make_shared<media::SfuEndpoint>(1106);
    auto audio = Negotiated("a", true, 111, 22);
    auto video = Negotiated("v", false, 102, 11);
    ASSERT_TRUE(source->ReplacePublishedTracks({audio, video}));
    ASSERT_TRUE(source->Start());
    ASSERT_TRUE(destination->Start());
    auto videoTransport = std::make_shared<CaptureTransport>();
    auto audioTransport = std::make_shared<CaptureTransport>();
    Configure(destination, "video", videoTransport, false, 777, 96);
    Configure(destination, "audio", audioTransport, true, 778, 109);
    ASSERT_TRUE(destination->Subscribe("video", source, "v"));
    ASSERT_TRUE(destination->Subscribe("audio", source, "a"));
    Input(source, Packet(11, 102));
    Input(source, Packet(22, 111));
    Drain(media_affinity::MakeStreamKey(destination->Id(), 777));
    Drain(media_affinity::MakeStreamKey(destination->Id(), 778));
    ASSERT_EQ(videoTransport->Count(), 1U);
    ASSERT_EQ(audioTransport->Count(), 1U);

    video.codecs.front().fmtp = "packetization-mode=1;profile-level-id=42e01e";
    EXPECT_FALSE(source->ReplacePublishedTracks({audio, video}));
    ::TrackInfo previous;
    ASSERT_TRUE(source->GetPublishedTrack("v", previous));
    EXPECT_TRUE(previous.fmtp.empty());
    EXPECT_EQ(destination->SubscriptionCount(), 2U);
    ASSERT_TRUE(destination->Unsubscribe("video"));
    ASSERT_TRUE(source->ReplacePublishedTracks({audio, video}));
    size_t changedVideoFrames = 0;
    source->AddEncodedFrameCallback([&](const auto& frame)
    {
        if (frame->info.track_id == static_cast<media::TrackId>(previous.track_index)) ++changedVideoFrames;
    });
    Input(source, Packet(11, 102, 1));
    EXPECT_EQ(changedVideoFrames, 1U);
    ASSERT_TRUE(destination->Subscribe("video", source, "v"));
    ASSERT_TRUE(source->ReplacePublishedTracks({audio}));
    EXPECT_EQ(destination->SubscriptionCount(), 1U);
    Input(source, Packet(11, 102, 2));
    Input(source, Packet(11, 111, 2)); // Retired camera SSRC cannot be learned as audio.
    Input(source, Packet(22, 111, 2));
    Drain(media_affinity::MakeStreamKey(destination->Id(), 777));
    Drain(media_affinity::MakeStreamKey(destination->Id(), 778));
    EXPECT_EQ(videoTransport->Count(), 1U);
    EXPECT_EQ(audioTransport->Count(), 2U);
}

TEST_F(SfuConference, ReplacePublishedTracksRejectsSubscribedSourceSwitchAtomically)
{
    auto source = std::make_shared<media::SfuEndpoint>(1107);
    auto destination = std::make_shared<media::SfuEndpoint>(1108);
    const auto audio = Negotiated("a", true, 111, 22);
    const auto video = Negotiated("v", false, 102, 11);
    ASSERT_TRUE(source->ReplacePublishedTracks({audio, video}));
    ASSERT_TRUE(source->Start());
    ASSERT_TRUE(destination->Start());
    auto transport = std::make_shared<CaptureTransport>();
    Configure(destination, "video", transport, false, 777, 96);
    ASSERT_TRUE(destination->Subscribe("video", source, "v"));
    Input(source, Packet(11, 102, 1000));
    Drain(media_affinity::MakeStreamKey(destination->Id(), 777));
    ASSERT_EQ(transport->Count(), 1U);

    auto changedAudio = audio;
    changedAudio.codecs.front().payloadType = 112;
    auto changedVideo = video;
    changedVideo.ssrcs = {{33, {}}};
    EXPECT_FALSE(source->ReplacePublishedTracks({changedAudio, changedVideo}));
    ::TrackInfo current;
    ASSERT_TRUE(source->GetPublishedTrack("a", current));
    EXPECT_EQ(current.payload_type, 111);
    EXPECT_EQ(source->PublishedTrackCount(), 2U);
    EXPECT_EQ(destination->SubscriptionCount(), 1U);
    Input(source, Packet(33, 102, 1));
    Input(source, Packet(11, 102, 1001));
    Drain(media_affinity::MakeStreamKey(destination->Id(), 777));
    ASSERT_EQ(transport->Count(), 2U);
    const auto last = transport->Last();
    EXPECT_EQ((static_cast<uint16_t>(last[2]) << 8) | last[3], 1001);
    EXPECT_EQ(media_affinity::ReadUint32BE(last.data() + 8), 777U);

    ASSERT_TRUE(destination->Unsubscribe("video"));
    EXPECT_TRUE(source->ReplacePublishedTracks({changedAudio, changedVideo}));
    ASSERT_TRUE(source->GetPublishedTrack("a", current));
    EXPECT_EQ(current.payload_type, 112);
}

TEST_F(SfuConference, SubscriberOwnsSenderAndFeedbackReturnsToPublisher)
{
    auto publisher = std::make_shared<media::SfuEndpoint>(1002);
    auto subscriber = std::make_shared<media::SfuEndpoint>(1003);
    ASSERT_TRUE(publisher->AddPublishedTrack("camera", Description(0, false, 102)));
    ASSERT_TRUE(publisher->Start());
    ASSERT_TRUE(subscriber->Start());
    auto transport = std::make_shared<CaptureTransport>();
    Configure(subscriber, "camera", transport, false, 777, 96);
    ASSERT_TRUE(subscriber->Subscribe("camera", publisher, "camera"));
    Input(publisher, Packet(11, 102));
    Drain(media_affinity::MakeStreamKey(subscriber->Id(), 777));
    ASSERT_EQ(transport->Count(), 1U);
    const auto packet = transport->Last();
    EXPECT_EQ(packet[1] & 127, 96);
    EXPECT_EQ(media_affinity::ReadUint32BE(packet.data() + 8), 777U);
    const auto nack = rtcpx::RtRtcpNack::Build(888, 777, {1});
    Input(subscriber, nack, {}, true);
    EXPECT_EQ(transport->Count(), 2U);
    Input(publisher, nack, {}, true); // Source endpoint does not own this sender.
    EXPECT_EQ(transport->Count(), 2U);
    auto pli_seen = std::make_shared<std::promise<uint32_t>>();
    auto pli = pli_seen->get_future();
    publisher->SetTrackRtcpSendCallback("camera", [pli_seen](const uint8_t* data, size_t len) {
        if (len >= 12 && data[1] == 206) pli_seen->set_value(media_affinity::ReadUint32BE(data + 8));
        return true;
    });
    Input(subscriber, rtcpx::RtcpFeedback::BuildPli(888, 777), {}, true);
    ASSERT_EQ(pli.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(pli.get(), 11U);
    ASSERT_TRUE(publisher->RemovePublishedTrack("camera"));
    EXPECT_EQ(subscriber->SubscriptionCount(), 0U);
    Input(subscriber, nack, {}, true);
    EXPECT_EQ(transport->Count(), 2U);
}

TEST_F(SfuConference, RoomRoutesMultipleTracksAndLeaveCleansOtherParticipants)
{
    room::RoomOptions options;
    options.auto_subscribe = false;
    room::Room conference({"test", "test"}, options);
    auto a = std::make_shared<room::Participant>("a", "A");
    auto b = std::make_shared<room::Participant>("b", "B");
    auto source = std::make_shared<media::SfuEndpoint>(1004);
    auto destination = std::make_shared<media::SfuEndpoint>(1005);
    ASSERT_TRUE(source->AddPublishedTrack("camera", Description(0, false, 102)));
    ASSERT_TRUE(source->AddPublishedTrack("mic", Description(1, true, 111)));
    ASSERT_TRUE(source->Start());
    ASSERT_TRUE(destination->Start());
    ASSERT_TRUE(a->BindEndpoint(source));
    ASSERT_TRUE(b->BindEndpoint(destination));
    ASSERT_TRUE(conference.Join(a));
    ASSERT_TRUE(conference.Join(b));
    a->OnTrackPublished([&](auto, auto) { EXPECT_EQ(conference.ParticipantCount(), 2U); });
    for (const auto& id : {"camera", "mic"}) {
        media::TrackInfo business;
        business.sid = id;
        ASSERT_TRUE(conference.PublishTrack("a", std::make_shared<media::MediaTrack>(business),
            std::string(id) == "camera" ? 11 : 22, std::string(id) == "camera" ? 102 : 111));
    }
    EXPECT_FALSE(conference.SubscribeTrack("b", "camera")); // No negotiated sender yet.
    auto transport = std::make_shared<CaptureTransport>();
    Configure(destination, "camera", transport, false, 777, 96);
    Configure(destination, "mic", transport, true, 778, 109);
    ASSERT_TRUE(conference.SubscribeTrack("b", "camera"));
    ASSERT_TRUE(conference.SubscribeTrack("b", "mic"));
    Input(source, Packet(11, 102));
    Input(source, Packet(22, 111));
    Drain(media_affinity::MakeStreamKey(destination->Id(), 777));
    Drain(media_affinity::MakeStreamKey(destination->Id(), 778));
    EXPECT_EQ(transport->Count(), 2U);
    ASSERT_TRUE(conference.UnpublishTrack("camera"));
    EXPECT_EQ(destination->SubscriptionCount(), 1U);
    EXPECT_FALSE(b->HasSubscribedTrack("camera"));
    Input(source, Packet(22, 111, 2));
    Drain(media_affinity::MakeStreamKey(destination->Id(), 778));
    EXPECT_EQ(transport->Count(), 3U);
    ASSERT_TRUE(conference.Leave("a"));
    EXPECT_TRUE(b->GetSubscribedTrackIds().empty());
    EXPECT_EQ(destination->SubscriptionCount(), 0U);
    EXPECT_FALSE(source->IsRunning());
    conference.Close();
    EXPECT_FALSE(destination->IsRunning());
}

TEST_F(SfuConference, QueuedPacketsDoNotOutliveSubscription)
{
    auto source = std::make_shared<media::SfuEndpoint>(1007);
    auto destination = std::make_shared<media::SfuEndpoint>(1008);
    ASSERT_TRUE(source->AddPublishedTrack("camera", Description(0, false, 102)));
    ASSERT_TRUE(source->Start());
    ASSERT_TRUE(destination->Start());
    auto transport = std::make_shared<CaptureTransport>();
    Configure(destination, "camera", transport, false, 777, 96);
    ASSERT_TRUE(destination->Subscribe("camera", source, "camera"));
    auto entered = std::make_shared<std::promise<void>>();
    auto release = std::make_shared<std::promise<void>>();
    auto waiting = release->get_future().share();
    auto ready = entered->get_future();
    ASSERT_EQ(WorkerService::post_fn(POOL_MEDIA, media_affinity::MakeStreamKey(destination->Id(), 777),
        [entered, waiting] { entered->set_value(); waiting.wait(); }), 0);
    ready.wait();
    Input(source, Packet(11, 102));
    EXPECT_TRUE(destination->Unsubscribe("camera"));
    EXPECT_TRUE(destination->Subscribe("camera", source, "camera"));
    Input(source, Packet(11, 102, 2));
    release->set_value();
    Drain(media_affinity::MakeStreamKey(destination->Id(), 777));
    ASSERT_EQ(transport->Count(), 1U);
    EXPECT_EQ(media_affinity::ReadUint16BE(transport->Last().data() + 2), 2U);
}

TEST_F(SfuConference, BothParticipantsPublishAndSubscribeWithoutCrossEndpointCalls)
{
    auto a = std::make_shared<media::SfuEndpoint>(1011);
    auto b = std::make_shared<media::SfuEndpoint>(1012);
    for (const auto& endpoint : {a, b}) {
        ASSERT_TRUE(endpoint->AddPublishedTrack("camera", Description(0, false, 102)));
        ASSERT_TRUE(endpoint->AddPublishedTrack("mic", Description(1, true, 111)));
        ASSERT_TRUE(endpoint->Start());
    }
    auto to_a = std::make_shared<CaptureTransport>();
    auto to_b = std::make_shared<CaptureTransport>();
    Configure(a, "camera", to_a, false, 777, 96);
    Configure(a, "mic", to_a, true, 778, 109);
    Configure(b, "camera", to_b, false, 779, 97);
    Configure(b, "mic", to_b, true, 780, 110);
    auto a_subscribes = std::async(std::launch::async, [&] {
        return a->Subscribe("camera", b, "camera") && a->Subscribe("mic", b, "mic");
    });
    auto b_subscribes = std::async(std::launch::async, [&] {
        return b->Subscribe("camera", a, "camera") && b->Subscribe("mic", a, "mic");
    });
    ASSERT_TRUE(a_subscribes.get());
    ASSERT_TRUE(b_subscribes.get());
    for (const auto& endpoint : {a, b}) {
        Input(endpoint, Packet(11, 102));
        Input(endpoint, Packet(22, 111));
    }
    Drain(media_affinity::MakeStreamKey(a->Id(), 777));
    Drain(media_affinity::MakeStreamKey(a->Id(), 778));
    Drain(media_affinity::MakeStreamKey(b->Id(), 779));
    Drain(media_affinity::MakeStreamKey(b->Id(), 780));
    EXPECT_EQ(to_a->Count(), 2U);
    EXPECT_EQ(to_b->Count(), 2U);
    a->Stop();
    EXPECT_EQ(a->SubscriptionCount(), 0U);
    EXPECT_EQ(b->SubscriptionCount(), 0U);
    EXPECT_EQ(b->PublishedTrackCount(), 2U);
    EXPECT_TRUE(b->IsRunning());
}

TEST_F(SfuConference, RebuildsDownstreamMidAndTwccInsteadOfCopyingUpstreamIds)
{
    auto source = std::make_shared<media::SfuEndpoint>(1009);
    auto destination = std::make_shared<media::SfuEndpoint>(1010);
    ASSERT_TRUE(source->AddPublishedTrack("camera", Description(0, false, 102), "v", 1));
    ASSERT_TRUE(source->Start());
    ASSERT_TRUE(destination->Start());
    auto transport = std::make_shared<CaptureTransport>();
    rtsp::RtpSenderTrackConfig config;
    config.local_ssrc = 777;
    config.payload_type = 96;
    config.mid = "dst";
    config.mid_extension_id = 3;
    config.transport_cc_extension_id = 5;
    config.transport_sequence_allocator = std::make_shared<media::TransportSequenceAllocator>();
    ASSERT_TRUE(destination->ConfigureSubscription("camera", config, transport, CodecId::H264));
    ASSERT_TRUE(destination->Subscribe("camera", source, "camera"));
    Input(source, Packet(11, 102, 1, "v"));
    Drain(media_affinity::MakeStreamKey(destination->Id(), 777));
    ASSERT_EQ(transport->Count(), 1U);
    const auto packet = transport->Last();
    ASSERT_GE(packet.size(), 27U);
    EXPECT_EQ(packet[16], 0x32); // MID ID 3, length 3.
    EXPECT_EQ(std::string(packet.begin() + 17, packet.begin() + 20), "dst");
    EXPECT_EQ(packet[20], 0x51); // TWCC ID 5, length 2.
    EXPECT_EQ(packet[24], 0x65); // Original payload preserved.
    Input(source, Packet(12, 102, 2, "v")); // A second encoding is not mixed into the selected one.
    Drain(media_affinity::MakeStreamKey(destination->Id(), 777));
    EXPECT_EQ(transport->Count(), 1U);
}

TEST(SfuSdpBinding, TypedWebRtcCodecsCreateRtpParametersAndMidBinding)
{
    sdp::SdpMedia media;
    media.media = "audio";
    media.mid = "a";
    media.codecs.push_back({111, "opus", 48000, 2, "minptime=10", {}});
    media.headerExtensions.push_back({1, "urn:ietf:params:rtp-hdrext:sdes:mid"});
    ::TrackInfo info;
    ASSERT_TRUE(media::BuildRtpTrackInfo(media, 2, info));
    EXPECT_EQ(info.codec_id, CodecId::OPUS);
    EXPECT_EQ(info.payload_type, 111);
    EXPECT_EQ(info.channels, 2);
    EXPECT_EQ(info.fmtp, "minptime=10");
    auto endpoint = std::make_shared<media::SfuEndpoint>(1006);
    ASSERT_TRUE(endpoint->AddPublishedTrack("mic", media, 2));
    ASSERT_TRUE(endpoint->Start());
    size_t frames = 0;
    endpoint->AddEncodedFrameCallback([&](const auto&) { ++frames; });
    Input(endpoint, Packet(22, 111, 1, "a"));
    EXPECT_EQ(frames, 1U);
}

TEST(SfuRoomCommands, SubscribeUsesTrackIdAndCloseDoesNotCreateAnotherRoom)
{
    auto manager = std::make_shared<room::RoomManager>();
    auto conference = manager->GetOrCreateRoom("conference", "test");
    auto a = std::make_shared<room::Participant>("a", "A");
    auto b = std::make_shared<room::Participant>("b", "B");
    ASSERT_TRUE(conference->Join(a));
    ASSERT_TRUE(conference->Join(b));
    media::TrackInfo info;
    info.sid = "camera";
    ASSERT_TRUE(conference->PublishTrack("a", std::make_shared<media::MediaTrack>(info), 11, 96));
    ASSERT_TRUE(conference->UnsubscribeTrack("b", "camera"));
    room::RoomCommandHandler handler(manager);
    room::RoomCommand command{};
    command.type = room::RoomCommandType::Subscribe;
    command.room_id = "conference";
    command.participant_id = "b";
    command.track_id = "camera";
    handler.Handle(command);
    EXPECT_TRUE(b->HasSubscribedTrack("camera"));
    command.type = room::RoomCommandType::Close;
    handler.Handle(command);
    handler.Handle(command);
    EXPECT_EQ(manager->FindRoom("conference"), conference);
    EXPECT_TRUE(conference->IsClosed());
    EXPECT_TRUE(b->GetSubscribedTrackIds().empty());
}
}
