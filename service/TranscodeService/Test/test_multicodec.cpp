#include "VideoCodecFactory.h"
#include "VideoTranscoder.h"
#include "TranscodeService.h"
#include "EncodedFrameRouter.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/log.h>
}

#include <gtest/gtest.h>
#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <stdexcept>

namespace {
namespace codec = media::codec;
using namespace service::transcode;
constexpr media::CodecType kCodecs[]{media::CodecType::H264, media::CodecType::H265,
    media::CodecType::VP8, media::CodecType::VP9, media::CodecType::AV1};
constexpr int kCount = 8;

const char* Name(media::CodecType type)
{
    switch (type) {
    case media::CodecType::H264: return "H264";
    case media::CodecType::H265: return "H265";
    case media::CodecType::VP8: return "VP8";
    case media::CodecType::VP9: return "VP9";
    case media::CodecType::AV1: return "AV1";
    default: return "Unknown";
    }
}

bool HasCodec(media::CodecType type, codec::CodecDirection direction)
{
    for (const auto& item : codec::VideoCodecFactory::Capabilities())
        if (item.codec == type && item.direction == direction && item.library_available) return true;
    return false;
}

void Require(bool success, const std::string& message)
{
    if (!success) throw std::runtime_error(message);
}

struct CpuStorage final : codec::VideoFrameStorage { std::vector<uint8_t> data; };
codec::RawVideoFrame Raw(int index)
{
    auto storage = std::make_shared<CpuStorage>();
    storage->data.resize(160 * 96 * 3 / 2, 128);
    // Nonuniform luma exercises real prediction and lossy reconstruction.
    for (int y = 0; y < 96; ++y)
        for (int x = 0; x < 160; ++x)
            storage->data[y * 160 + x] = 70 + ((x / 16 + y / 16 + index) % 60);
    codec::RawVideoFrame frame;
    frame.storage = storage;
    frame.width = 160; frame.height = 96;
    frame.planes = {storage->data.data(), storage->data.data() + 160 * 96,
                    storage->data.data() + 160 * 96 * 5 / 4, nullptr};
    frame.strides = {160, 80, 80, 0};
    frame.timestamp.time_base_den = 90000;
    frame.timestamp.pts = 9000 + index * 3000;
    return frame;
}

const std::vector<media::EncodedFrame::ConstPtr>& Stream(media::CodecType type)
{
    static std::map<media::CodecType, std::vector<media::EncodedFrame::ConstPtr>> cache;
    auto& result = cache[type];
    if (!result.empty()) return result;
    av_log_set_level(AV_LOG_ERROR);
    codec::VideoEncoderConfig config;
    config.codec = type; config.width = 160; config.height = 96;
    config.bitrate_bps = 200000; config.keyframe_interval = 4;
    codec::CodecError error;
    auto encoder = codec::VideoCodecFactory::CreateEncoder(config, {}, error);
    Require(bool(encoder), error.message);
    std::vector<media::EncodedFrame::Ptr> encoded;
    for (int i = 0; i < kCount; ++i)
        Require(encoder->Encode(Raw(i), encoded, error), error.message);
    Require(encoder->Flush(encoded, error), error.message);
    Require(encoded.size() == kCount, "encoder did not produce one picture per input");
    for (auto& frame : encoded) {
        frame->info.track_id = 42;
        result.push_back(std::move(frame));
    }
    return result;
}

// Uses independent FFmpeg contexts, an explicit expected codec ID and the
// actual bytes. It does not trust our frame labels or reuse the adapter decoder.
void VerifyBytes(const std::vector<media::EncodedFrame::ConstPtr>& frames,
                 media::CodecType expected, int width, int height, int expected_profile = -1)
{
    AVCodecID id = AV_CODEC_ID_NONE;
    switch (expected) {
    case media::CodecType::H264: id = AV_CODEC_ID_H264; break;
    case media::CodecType::H265: id = AV_CODEC_ID_HEVC; break;
    case media::CodecType::VP8: id = AV_CODEC_ID_VP8; break;
    case media::CodecType::VP9: id = AV_CODEC_ID_VP9; break;
    case media::CodecType::AV1: id = AV_CODEC_ID_AV1; break;
    default: FAIL() << "unexpected codec";
    }
    const auto* impl = expected == media::CodecType::AV1
        ? avcodec_find_decoder_by_name("libdav1d") : avcodec_find_decoder(id);
    if (!impl) impl = avcodec_find_decoder_by_name("libaom-av1");
    ASSERT_NE(impl, nullptr);
    ASSERT_EQ(impl->id, id);
    auto context_deleter = [](AVCodecContext* p) { avcodec_free_context(&p); };
    auto frame_deleter = [](AVFrame* p) { av_frame_free(&p); };
    auto packet_deleter = [](AVPacket* p) { av_packet_free(&p); };
    std::unique_ptr<AVCodecContext, decltype(context_deleter)> decoder(avcodec_alloc_context3(impl), context_deleter);
    std::unique_ptr<AVFrame, decltype(frame_deleter)> raw(av_frame_alloc(), frame_deleter);
    std::unique_ptr<AVPacket, decltype(packet_deleter)> packet(av_packet_alloc(), packet_deleter);
    ASSERT_TRUE(decoder && raw && packet);
    decoder->thread_count = 1;
    decoder->pkt_timebase = {1, 90000};
    decoder->err_recognition = AV_EF_EXPLODE;
    ASSERT_GE(avcodec_open2(decoder.get(), impl, nullptr), 0);
    std::vector<int64_t> decoded_pts;
    auto drain = [&] {
        for (;;) {
            const int ret = avcodec_receive_frame(decoder.get(), raw.get());
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return;
            ASSERT_GE(ret, 0);
            EXPECT_EQ(raw->width, width);
            EXPECT_EQ(raw->height, height);
            EXPECT_EQ(raw->decode_error_flags, 0);
            EXPECT_EQ(raw->flags & AV_FRAME_FLAG_CORRUPT, 0);
            if (expected_profile >= 0) EXPECT_EQ(decoder->profile & 255, expected_profile);
            // Avoid accepting an empty/black result as a successful conversion.
            EXPECT_GT(raw->data[0][raw->linesize[0] * height / 2 + width / 2], 40);
            decoded_pts.push_back(raw->pts);
            av_frame_unref(raw.get());
        }
    };
    for (const auto& frame : frames) {
        ASSERT_GE(av_new_packet(packet.get(), static_cast<int>(frame->Size())), 0);
        std::memcpy(packet->data, frame->Data(), frame->Size());
        packet->pts = frame->info.timestamp.pts;
        packet->dts = frame->info.timestamp.dts;
        ASSERT_GE(avcodec_send_packet(decoder.get(), packet.get()), 0);
        av_packet_unref(packet.get());
        drain();
    }
    ASSERT_GE(avcodec_send_packet(decoder.get(), nullptr), 0);
    drain();
    ASSERT_EQ(decoded_pts.size(), frames.size());
    for (size_t i = 0; i < frames.size(); ++i) EXPECT_EQ(decoded_pts[i], frames[i]->info.timestamp.pts);
}

class MultiCodec : public testing::TestWithParam<media::CodecType> {};

TEST_P(MultiCodec, ProducesRealBitstreamAndAllAvailableDecodersReadIt)
{
    const auto type = GetParam();
    if (!HasCodec(type, codec::CodecDirection::Encode)) GTEST_SKIP() << "encoder unavailable: " << Name(type);
    const auto& frames = Stream(type);
    ASSERT_EQ(frames.size(), kCount);
    ASSERT_TRUE(frames.front()->IsKeyFrame());
    int keyframes = 0;
    for (size_t i = 0; i < frames.size(); ++i) {
        EXPECT_EQ(frames[i]->info.codec, type);
        EXPECT_EQ(frames[i]->bitstream, codec::BitstreamFor(type));
        EXPECT_EQ(frames[i]->info.timestamp.pts, 9000 + int64_t(i) * 3000);
        if (frames[i]->IsKeyFrame()) {
            ++keyframes;
            if (type == media::CodecType::H264 || type == media::CodecType::H265) {
                EXPECT_TRUE(frames[i]->video.has_sps);
                EXPECT_TRUE(frames[i]->video.has_pps);
                if (type == media::CodecType::H265) EXPECT_TRUE(frames[i]->video.has_vps);
            } else EXPECT_FALSE(frames[i]->video.is_idr);
            VerifyBytes({frames.begin() + i, frames.end()}, type, 160, 96);
        }
    }
    EXPECT_GE(keyframes, 2);
    VerifyBytes(frames, type, 160, 96);
    for (const auto& capability : codec::VideoCodecFactory::Capabilities()) {
        if (capability.codec != type || capability.direction != codec::CodecDirection::Decode || !capability.library_available) continue;
        SCOPED_TRACE(capability.implementation);
        codec::CodecError error;
        auto decoder = codec::VideoCodecFactory::CreateDecoder({type},
            {codec::SelectionMode::Require, capability.implementation}, error);
        ASSERT_TRUE(decoder) << error.message;
        std::vector<codec::RawVideoFrame> raw;
        for (const auto& frame : frames) ASSERT_TRUE(decoder->Decode(*frame, raw, error)) << error.message;
        ASSERT_TRUE(decoder->Flush(raw, error)) << error.message;
        ASSERT_TRUE(decoder->Flush(raw, error));
        decoder.reset();
        ASSERT_EQ(raw.size(), frames.size());
        for (size_t i = 0; i < raw.size(); ++i) {
            EXPECT_EQ(raw[i].timestamp.pts, frames[i]->info.timestamp.pts);
            EXPECT_EQ(raw[i].width, 160);
            EXPECT_GT(raw[i].planes[0][0], 40);
        }
    }
}

TEST_P(MultiCodec, OneDecoderFeedsFiveDifferentOutputFormats)
{
    const auto type = GetParam();
    for (auto output : kCodecs)
        if (!HasCodec(output, codec::CodecDirection::Encode)) GTEST_SKIP() << "output encoder unavailable: " << Name(output);
    if (!HasCodec(type, codec::CodecDirection::Decode)) GTEST_SKIP() << "input decoder unavailable";
    VideoTranscodeConfig config;
    config.input_codec = type;
    config.keyframe_interval = 4;
    config.renditions.clear();
    for (size_t i = 0; i < std::size(kCodecs); ++i) {
        VideoRendition out{Name(kCodecs[i]), media::TrackId(i + 1), {128, 72, 150000}};
        out.codec = kCodecs[i];
        config.renditions.push_back(std::move(out));
    }
    VideoTranscoder transcoder;
    ASSERT_TRUE(transcoder.Open(config)) << transcoder.LastError();
    std::vector<TranscodedFrame> outputs;
    for (const auto& input : Stream(type)) ASSERT_TRUE(transcoder.Input(*input, outputs)) << transcoder.LastError();
    ASSERT_TRUE(transcoder.Flush(outputs)) << transcoder.LastError();
    EXPECT_EQ(transcoder.Stats().decoded_frames, kCount);
    EXPECT_EQ(transcoder.Stats().encoded_frames, kCount * std::size(kCodecs));
    transcoder.Close();
    std::map<std::string, std::vector<media::EncodedFrame::ConstPtr>> grouped;
    for (const auto& output : outputs) grouped[output.rendition_id].push_back(output.frame);
    ASSERT_EQ(grouped.size(), std::size(kCodecs));
    for (size_t i = 0; i < std::size(kCodecs); ++i) {
        SCOPED_TRACE(Name(kCodecs[i]));
        const auto& frames = grouped.at(Name(kCodecs[i]));
        ASSERT_EQ(frames.size(), kCount);
        for (const auto& frame : frames) {
            EXPECT_EQ(frame->info.codec, kCodecs[i]);
            EXPECT_EQ(frame->info.track_id, i + 1);
            EXPECT_EQ(frame->rtp.ssrc, 0u);
        }
        VerifyBytes(frames, kCodecs[i], 128, 72);
    }
}

class Collector final : public media::IEncodedFrameSink
{
public:
    bool SubmitFrame(const media::EncodedFrameEvent& event) override
    {
        frames.push_back(event.frame); // Inspected after the worker is joined.
        return true;
    }
    std::vector<media::EncodedFrame::ConstPtr> frames;
};

TEST_P(MultiCodec, ServiceRoutesConfiguredInputAndWaitsForRandomAccessFrame)
{
    const auto type = GetParam();
    if (!HasCodec(type, codec::CodecDirection::Encode) || !HasCodec(type, codec::CodecDirection::Decode))
        GTEST_SKIP() << "codec unavailable";
    auto input = std::make_shared<media::EncodedFrameRouter>();
    auto output = std::make_shared<media::EncodedFrameRouter>();
    auto sink = std::make_shared<Collector>();
    output->Subscribe(sink);
    TranscodeService service(input, output);
    ASSERT_TRUE(service.Init());
    ASSERT_TRUE(service.Start());
    TranscodeTaskConfig config;
    config.task_id = "multi";
    config.input = {10, 42};
    config.output = {20, "session", "stream"};
    config.video.timestamp_mode = InputTimestampMode::MediaTime;
    config.video.input_codec = type;
    config.video.renditions = {{"out", 1, {160, 96, 200000}}};
    config.video.renditions.front().codec = type;
    ASSERT_TRUE(service.StartTask(config)) << service.LastError();
    const auto& frames = Stream(type);
    media::EncodedFrameEvent event;
    event.source.endpoint_id = 10; event.source.track_id = 42;
    auto wrong = std::make_shared<media::EncodedFrame>(*frames.front());
    wrong->info.codec = media::CodecType::AAC;
    event.frame = wrong;
    EXPECT_EQ(input->Publish(event), 0u);
    auto delta = std::find_if(frames.begin(), frames.end(), [](const auto& frame) { return !frame->IsKeyFrame(); });
    ASSERT_NE(delta, frames.end());
    event.frame = *delta;
    EXPECT_EQ(input->Publish(event), 0u);
    for (const auto& frame : frames) {
        event.frame = frame;
        EXPECT_EQ(input->Publish(event), 1u);
    }
    TranscodeTaskStatus final;
    ASSERT_TRUE(service.StopTask("multi", &final));
    EXPECT_EQ(final.state, TranscodeTaskState::Stopped) << final.last_error;
    EXPECT_EQ(final.stats.decoded_frames, kCount);
    EXPECT_EQ(final.stats.encoded_frames, kCount);
    VerifyBytes(sink->frames, type, 160, 96);
}

INSTANTIATE_TEST_SUITE_P(Formats, MultiCodec, testing::ValuesIn(kCodecs),
    [](const testing::TestParamInfo<media::CodecType>& info) { return Name(info.param); });

TEST(MultiCodecSelection, RejectsMismatchedParametersRepresentationAndStrictImplementation)
{
    codec::CodecError error;
    codec::VideoEncoderConfig config;
    config.codec = media::CodecType::VP9;
    config.width = 160; config.height = 96; config.bitrate_bps = 200000;
    config.parameters = codec::H264Parameters{};
    EXPECT_FALSE(codec::VideoCodecFactory::CreateEncoder(config, {}, error));
    EXPECT_EQ(error.code, codec::CodecErrorCode::InvalidConfiguration);
    config.parameters = codec::Vp9Parameters{2};
    EXPECT_FALSE(codec::VideoCodecFactory::CreateEncoder(config, {}, error));
    EXPECT_EQ(error.code, codec::CodecErrorCode::Unsupported);
    config.parameters = codec::Vp9Parameters{};
    EXPECT_FALSE(codec::VideoCodecFactory::CreateEncoder(config,
        {codec::SelectionMode::Require, "libx264"}, error));
    EXPECT_EQ(error.code, codec::CodecErrorCode::Unsupported);
    auto encoder = codec::VideoCodecFactory::CreateEncoder(config,
        {codec::SelectionMode::Prefer, "libx264"}, error);
    ASSERT_TRUE(encoder) << error.message;
    EXPECT_STREQ(encoder->Implementation(), "libvpx-vp9");
    auto decoder = codec::VideoCodecFactory::CreateDecoder({media::CodecType::VP9}, {}, error);
    ASSERT_TRUE(decoder) << error.message;
    auto wrong = *Stream(media::CodecType::VP9).front();
    wrong.bitstream = media::EncodedBitstream::AnnexB;
    std::vector<codec::RawVideoFrame> raw;
    EXPECT_FALSE(decoder->Decode(wrong, raw, error));
    EXPECT_NE(error.message.find("representation"), std::string::npos);
}

TEST(MultiCodecSelection, FailedInitializationRollsBackAndTaskCanRetryWithExplicitBackend)
{
    auto input = std::make_shared<media::EncodedFrameRouter>();
    auto output = std::make_shared<media::EncodedFrameRouter>();
    TranscodeService service(input, output, 1);
    ASSERT_TRUE(service.Init()); ASSERT_TRUE(service.Start());
    TranscodeTaskConfig config;
    config.task_id = "retry"; config.input = {10, 42}; config.output = {20, "session", "stream"};
    config.video.renditions.front().backend = {codec::SelectionMode::Require, "not-registered"};
    EXPECT_FALSE(service.StartTask(config));
    EXPECT_FALSE(service.TaskStatus("retry"));
    EXPECT_NE(service.LastError().find("not-registered"), std::string::npos);
    config.video.decoder = {codec::SelectionMode::Require, "h264"};
    config.video.renditions.front().backend = {codec::SelectionMode::Require, "libx264"};
    ASSERT_TRUE(service.StartTask(config)) << service.LastError();
    EXPECT_TRUE(service.StopTask("retry"));
}

TEST(MultiCodecSelection, H264ProfileParametersChangeTheActualBitstream)
{
    for (const auto profile : {codec::H264Profile::Baseline, codec::H264Profile::Main, codec::H264Profile::High}) {
        codec::VideoEncoderConfig config;
        config.width = 160; config.height = 96; config.bitrate_bps = 200000;
        config.parameters = codec::H264Parameters{profile};
        codec::CodecError error;
        auto encoder = codec::VideoCodecFactory::CreateEncoder(config, {}, error);
        ASSERT_TRUE(encoder) << error.message;
        std::vector<media::EncodedFrame::Ptr> encoded;
        for (int i = 0; i < 3; ++i) ASSERT_TRUE(encoder->Encode(Raw(i), encoded, error)) << error.message;
        ASSERT_TRUE(encoder->Flush(encoded, error)) << error.message;
        const int expected_profile = profile == codec::H264Profile::Baseline ? 66 :
            (profile == codec::H264Profile::Main ? 77 : 100);
        VerifyBytes({encoded.begin(), encoded.end()}, media::CodecType::H264, 160, 96, expected_profile);
    }
}

TEST(MultiCodecSelection, InitializationFailureReportsCandidateAndDoesNotPoisonNextCreation)
{
    bool svt_available = false;
    for (const auto& item : codec::VideoCodecFactory::Capabilities())
        svt_available |= item.direction == codec::CodecDirection::Encode &&
            item.implementation == "libsvtav1" && item.library_available;
    if (!svt_available) GTEST_SKIP() << "SVT-AV1 unavailable";
    codec::VideoEncoderConfig config;
    config.codec = media::CodecType::AV1;
    config.width = 32; config.height = 32; config.bitrate_bps = 200000;
    codec::CodecError error;
    // SVT has stricter dimensions than the common video configuration.
    EXPECT_FALSE(codec::VideoCodecFactory::CreateEncoder(config,
        {codec::SelectionMode::Require, "libsvtav1"}, error));
    EXPECT_EQ(error.code, codec::CodecErrorCode::InitializationFailed);
    EXPECT_NE(error.message.find("libsvtav1"), std::string::npos);
    config.width = 160; config.height = 96;
    auto encoder = codec::VideoCodecFactory::CreateEncoder(config,
        {codec::SelectionMode::Prefer, "libaom-av1"}, error);
    ASSERT_TRUE(encoder) << error.message;
    std::vector<media::EncodedFrame::Ptr> output;
    ASSERT_TRUE(encoder->Encode(Raw(0), output, error)) << error.message;
    ASSERT_TRUE(encoder->Flush(output, error)) << error.message;
    ASSERT_EQ(output.size(), 1u);
    VerifyBytes({output.begin(), output.end()}, media::CodecType::AV1, 160, 96);
}
} // namespace
