#include <gtest/gtest.h>

#include "AudioDepacketizer.h"
#include "AudioRtpDepacketizerFactory.h"

namespace
{
RtpView MakeView(const std::vector<uint8_t>& payload,
                 uint16_t seq,
                 uint32_t timestamp,
                 bool marker = true)
{
    RtpView view;
    view.ssrc = 0x11223344;
    view.seq = seq;
    view.ts = timestamp;
    view.marker = marker;
    view.payload = payload.data();
    view.payload_len = payload.size();
    return view;
}
}

TEST(AudioDepacketizerTest, ParsesSingleAacHbrAccessUnit)
{
    media::AudioDepacketizer depacketizer(
        media::CodecType::AAC,
        48000,
        2,
        "mode=AAC-hbr; sizeLength=13; indexLength=3; indexDeltaLength=3");
    const std::vector<uint8_t> payload{0x00, 0x10, 0x00, 0x20,
                                       0x11, 0x22, 0x33, 0x44};

    ASSERT_TRUE(depacketizer.Input(MakeView(payload, 10, 1000)));
    media::EncodedFrame frame;
    ASSERT_TRUE(depacketizer.PopFrame(frame));
    ASSERT_EQ(frame.size, 4U);
    EXPECT_EQ(frame.sample_count, 1024U);
    EXPECT_EQ(frame.info.timestamp.pts, 1000);
    EXPECT_EQ(std::vector<uint8_t>(frame.Data(), frame.Data() + frame.size),
              (std::vector<uint8_t>{0x11, 0x22, 0x33, 0x44}));
}

TEST(AudioDepacketizerTest, ParsesMultipleAacAccessUnits)
{
    media::AudioDepacketizer depacketizer(
        media::CodecType::AAC,
        48000,
        2,
        "sizeLength=13; indexLength=3; indexDeltaLength=3; constantDuration=1024");
    const std::vector<uint8_t> payload{0x00, 0x20,
                                       0x00, 0x18,
                                       0x00, 0x10,
                                       1, 2, 3, 4, 5};

    ASSERT_TRUE(depacketizer.Input(MakeView(payload, 20, 2000)));
    media::EncodedFrame first;
    media::EncodedFrame second;
    ASSERT_TRUE(depacketizer.PopFrame(first));
    ASSERT_TRUE(depacketizer.PopFrame(second));
    EXPECT_EQ(first.size, 3U);
    EXPECT_EQ(second.size, 2U);
    EXPECT_EQ(first.info.timestamp.pts, 2000);
    EXPECT_EQ(second.info.timestamp.pts, 3024);
}

TEST(AudioDepacketizerTest, ReassemblesFragmentedAacAccessUnit)
{
    media::AudioDepacketizer depacketizer(
        media::CodecType::AAC,
        44100,
        2,
        "sizeLength=13; indexLength=3; indexDeltaLength=3");
    const std::vector<uint8_t> first_payload{0x00, 0x10, 0x00, 0x30, 1, 2, 3};
    const std::vector<uint8_t> last_payload{0x00, 0x10, 0x00, 0x30, 4, 5, 6};

    ASSERT_TRUE(depacketizer.Input(MakeView(first_payload, 30, 3000, false)));
    EXPECT_FALSE(depacketizer.HasFrame());
    ASSERT_TRUE(depacketizer.Input(MakeView(last_payload, 31, 3000, true)));

    media::EncodedFrame frame;
    ASSERT_TRUE(depacketizer.PopFrame(frame));
    EXPECT_EQ(frame.rtp.first_sequence, 30);
    EXPECT_EQ(frame.rtp.last_sequence, 31);
    EXPECT_EQ(std::vector<uint8_t>(frame.Data(), frame.Data() + frame.size),
              (std::vector<uint8_t>{1, 2, 3, 4, 5, 6}));
}

TEST(AudioDepacketizerTest, RejectsBrokenAacHeader)
{
    media::AudioDepacketizer depacketizer(media::CodecType::AAC, 48000, 2);
    const std::vector<uint8_t> payload{0x00, 0x20, 0x00, 0x20};
    EXPECT_FALSE(depacketizer.Input(MakeView(payload, 40, 4000)));
    EXPECT_FALSE(depacketizer.HasFrame());
}

TEST(AudioRtpDepacketizerFactoryTest, CreatesG711Depacketizer)
{
    auto depacketizer = media::AudioRtpDepacketizerFactory::Create(
        media::CodecType::PCMU, "PCMU", 8000, 1);
    ASSERT_NE(depacketizer, nullptr);

    const std::vector<uint8_t> payload(160, 0x7f);
    ASSERT_TRUE(depacketizer->Input(MakeView(payload, 50, 8000)));
    media::EncodedFrame frame;
    ASSERT_TRUE(depacketizer->PopFrame(frame));
    EXPECT_EQ(frame.info.codec, media::CodecType::PCMU);
    EXPECT_EQ(frame.sample_count, 160U);
}

TEST(AudioRtpDepacketizerFactoryTest, CreatesOpusDepacketizer)
{
    auto depacketizer = media::AudioRtpDepacketizerFactory::Create(
        media::CodecType::Opus, "opus", 48000, 2);
    ASSERT_NE(depacketizer, nullptr);

    const std::vector<uint8_t> payload{0x78, 0x01, 0x02};
    ASSERT_TRUE(depacketizer->Input(MakeView(payload, 51, 48000)));
    media::EncodedFrame frame;
    ASSERT_TRUE(depacketizer->PopFrame(frame));
    EXPECT_EQ(frame.info.codec, media::CodecType::Opus);
    EXPECT_EQ(frame.sample_rate, 48000U);
    EXPECT_EQ(frame.channels, 2U);
    EXPECT_EQ(frame.sample_count, 960U);
}

TEST(AudioRtpDepacketizerFactoryTest, RejectsMalformedOpusPacket)
{
    auto depacketizer = media::AudioRtpDepacketizerFactory::Create(
        media::CodecType::Opus, "opus", 48000, 2);
    ASSERT_NE(depacketizer, nullptr);

    // Code 3 requires a second byte carrying the frame count.
    const std::vector<uint8_t> payload{0x7b};
    EXPECT_FALSE(depacketizer->Input(MakeView(payload, 52, 48960)));
    EXPECT_FALSE(depacketizer->HasFrame());
}

TEST(AudioRtpDepacketizerFactoryTest, RejectsLatmUntilImplemented)
{
    auto depacketizer = media::AudioRtpDepacketizerFactory::Create(
        media::CodecType::AAC, "MP4A-LATM", 48000, 2);
    EXPECT_EQ(depacketizer, nullptr);
}
