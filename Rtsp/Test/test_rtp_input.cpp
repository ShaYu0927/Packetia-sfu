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
    ASSERT_NE(nullptr, source->inputRtp(TrackAudio, 48000, first.data(), first.size()));

    EXPECT_EQ(1u, source->getStats().received_packets);
    EXPECT_EQ(1u, clone->getStats().received_packets);
    EXPECT_EQ(100u, source->getStats().last_seq);
    EXPECT_EQ(100u, clone->getStats().last_seq);

    // Direct input into the clone changes only the clone's receiver state.
    auto second = MakeAudioRtp(101, 48960);
    ASSERT_NE(nullptr, clone->inputRtp(TrackAudio, 48000, second.data(), second.size()));

    EXPECT_EQ(1u, source->getStats().received_packets);
    EXPECT_EQ(2u, clone->getStats().received_packets);
    EXPECT_EQ(100u, source->getStats().last_seq);
    EXPECT_EQ(101u, clone->getStats().last_seq);
}

TEST(RtpAudioTrackerCloneTest, ReleasedCloneDetachesAutomatically)
{
    auto source = std::make_shared<rtsp::RtpAudioTracker>(MakeAudioTrackInfo());
    {
        auto clone = source->Clone();
        ASSERT_TRUE(clone);
    }

    auto packet = MakeAudioRtp(200, 96000);
    ASSERT_NE(nullptr, source->inputRtp(TrackAudio, 48000, packet.data(), packet.size()));
    EXPECT_EQ(1u, source->getStats().received_packets);
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}



