#include <gtest/gtest.h>

#include "RtpReceiver.h"

#include <array>

namespace
{
TrackInfo MakeAudioTrackInfo()
{
    TrackInfo info;
    info.type = TrackAudio;
    info.codec_id = CodecId::OPUS;
    info.codec_name = "OPUS";
    info.track_index = 1;
    info.ssrc = 0x01020304;
    info.clock_rate = 48000;
    info.payload_type = 111;
    info.channels = 2;
    return info;
}

std::array<uint8_t, 16> MakeAudioRtp(uint16_t seq, uint32_t timestamp)
{
    std::array<uint8_t, 16> packet{};
    packet[0] = 0x80;
    packet[1] = 111;
    packet[2] = static_cast<uint8_t>(seq >> 8);
    packet[3] = static_cast<uint8_t>(seq);
    packet[4] = static_cast<uint8_t>(timestamp >> 24);
    packet[5] = static_cast<uint8_t>(timestamp >> 16);
    packet[6] = static_cast<uint8_t>(timestamp >> 8);
    packet[7] = static_cast<uint8_t>(timestamp);
    packet[8] = 0x01;
    packet[9] = 0x02;
    packet[10] = 0x03;
    packet[11] = 0x04;
    packet[12] = 0xF8;
    packet[13] = 0xFF;
    packet[14] = 0xFE;
    packet[15] = 0x00;
    return packet;
}
}

TEST(RtpAudioTrackerCloneTest, MirrorsFuturePacketsWithIndependentState)
{
    auto source = std::make_shared<rtsp::RtpAudioTracker>(MakeAudioTrackInfo());
    auto clone = source->Clone();

    ASSERT_NE(source.get(), clone.get());
    EXPECT_EQ(source->getTrackInfo().codec_id, clone->getTrackInfo().codec_id);
    EXPECT_EQ(source->getTrackInfo().clock_rate, clone->getTrackInfo().clock_rate);
    EXPECT_EQ(source->getTrackInfo().channels, clone->getTrackInfo().channels);
    EXPECT_EQ(0u, source->getStats().received_packets);
    EXPECT_EQ(0u, clone->getStats().received_packets);

    auto first = MakeAudioRtp(100, 48000);
    ASSERT_NE(nullptr, source->inputRtp(first.data(), first.size()));

    EXPECT_EQ(1u, source->getStats().received_packets);
    EXPECT_EQ(1u, clone->getStats().received_packets);
    EXPECT_EQ(100u, source->getStats().last_seq);
    EXPECT_EQ(100u, clone->getStats().last_seq);

    // Direct input into the clone changes only the clone's receiver state.
    auto second = MakeAudioRtp(101, 48960);
    ASSERT_NE(nullptr, clone->inputRtp(second.data(), second.size()));

    EXPECT_EQ(1u, source->getStats().received_packets);
    EXPECT_EQ(2u, clone->getStats().received_packets);
    EXPECT_EQ(100u, source->getStats().last_seq);
    EXPECT_EQ(101u, clone->getStats().last_seq);
}

TEST(RtpReceiverTrackTest, FactoryCreatesOnlyImplementedReceiverCodecs)
{
    auto audio = rtsp::RtpReceiverTrack::Create(MakeAudioTrackInfo());
    ASSERT_NE(audio, nullptr);
    EXPECT_NE(std::dynamic_pointer_cast<rtsp::RtpAudioTracker>(audio), nullptr);

    TrackInfo video;
    video.type = TrackVideo;
    video.codec_id = CodecId::H264;
    video.clock_rate = 90000;
    EXPECT_NE(rtsp::RtpReceiverTrack::Create(video), nullptr);

    video.codec_id = CodecId::H265;
    EXPECT_EQ(rtsp::RtpReceiverTrack::Create(video), nullptr);
}

TEST(RtpReceiverTrackTest, RejectsPacketOutsideNegotiatedIdentity)
{
    auto receiver = rtsp::RtpReceiverTrack::Create(MakeAudioTrackInfo());
    ASSERT_NE(receiver, nullptr);

    auto wrong_payload_type = MakeAudioRtp(1, 48000);
    wrong_payload_type[1] = 112;
    EXPECT_EQ(receiver->inputRtp(wrong_payload_type.data(), wrong_payload_type.size()), nullptr);

    auto wrong_ssrc = MakeAudioRtp(2, 48960);
    wrong_ssrc[11] = 0x05;
    EXPECT_EQ(receiver->inputRtp(wrong_ssrc.data(), wrong_ssrc.size()), nullptr);
    EXPECT_EQ(receiver->getStats().received_packets, 0U);
}

TEST(RtpAudioTrackerCloneTest, ReleasedCloneDetachesAutomatically)
{
    auto source = std::make_shared<rtsp::RtpAudioTracker>(MakeAudioTrackInfo());
    {
        auto clone = source->Clone();
        ASSERT_TRUE(clone);
    }

    auto packet = MakeAudioRtp(200, 96000);
    ASSERT_NE(nullptr, source->inputRtp(packet.data(), packet.size()));
    EXPECT_EQ(1u, source->getStats().received_packets);
}

TEST(RtpPacketPoolTest, IsBoundedAndReusesPacketStorage)
{
    auto pool = std::make_shared<rtsp::RtpPacketPool>(2, 1500);
    EXPECT_EQ(2u, pool->Capacity());
    EXPECT_EQ(2u, pool->Available());

    auto first = pool->Acquire();
    auto second = pool->Acquire();
    ASSERT_NE(nullptr, first);
    ASSERT_NE(nullptr, second);
    EXPECT_EQ(0u, pool->Available());
    EXPECT_EQ(nullptr, pool->Acquire());

    auto* released_address = first.get();
    first.reset();
    EXPECT_EQ(1u, pool->Available());

    auto reused = pool->Acquire();
    ASSERT_NE(nullptr, reused);
    EXPECT_EQ(released_address, reused.get());
    EXPECT_GE(reused->getCapacity(), 1500u);
}

TEST(RtpPacketPoolTest, RejectsOversizedPacketBeforeAllocation)
{
    auto tracker = std::make_shared<rtsp::RtpAudioTracker>(MakeAudioTrackInfo());
    std::array<uint8_t, RtpPacket::kRtpMaxSize + 1> oversized{};
    oversized[0] = 0x80;
    oversized[1] = 111;

    EXPECT_EQ(nullptr, tracker->inputRtp(oversized.data(), oversized.size()));
    EXPECT_EQ(1u, tracker->getStats().oversized_packets);
    EXPECT_EQ(0u, tracker->getStats().received_packets);
}

TEST(RtpPacketSortorTest, ReorderBufferNeverExceedsConfiguredLimit)
{
    EnhancedPacketSortor<std::shared_ptr<int>, uint16_t> sorter(1000, 8, 100);
    sorter.setOnPacketSorted([](uint16_t, std::shared_ptr<int>) {});
    sorter.inputPacket(0, std::make_shared<int>(0));

    for (uint16_t seq = 100; seq < 140; ++seq)
    {
        sorter.inputPacket(seq, std::make_shared<int>(seq));
        EXPECT_LE(sorter.getBufferedCount(), 8u);
    }
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
