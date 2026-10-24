#include "VideoTranscoder.h"
#include "VideoCodecFactory.h"
#include "TranscodeService.h"
#include "TranscodeTask.h"
#include "EncodedFrameRouter.h"
#include "ThreadManager.h"
#ifdef PACKETIA_TEST_SFU_PIPELINE
#include "SfuEndpoint.h"
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/dict.h>
#include <libavutil/log.h>
}

#include <gtest/gtest.h>
#include <algorithm>
#include <cstring>
#include <map>
#include <chrono>
#include <condition_variable>
#include <atomic>
#include <stdexcept>

namespace {
using namespace service::transcode;
struct CodecDeleter { void operator()(AVCodecContext* p) const { avcodec_free_context(&p); } };
struct FrameDeleter { void operator()(AVFrame* p) const { av_frame_free(&p); } };
struct PacketDeleter { void operator()(AVPacket* p) const { av_packet_free(&p); } };
using CodecPtr = std::unique_ptr<AVCodecContext, CodecDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;

void Require(bool condition)
{
    if (!condition) throw std::runtime_error("FFmpeg fixture/verification operation failed");
}

std::vector<media::EncodedFrame> MakeInput(int width, int height, int count, int bframes = 0,
                                         int64_t origin = 9000, bool rtp32 = false)
{
    av_log_set_level(AV_LOG_ERROR);
    const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
    Require(codec != nullptr);
    CodecPtr encoder(avcodec_alloc_context3(codec));
    FramePtr frame(av_frame_alloc());
    PacketPtr packet(av_packet_alloc());
    Require(encoder && frame && packet);
    encoder->width = width;
    encoder->height = height;
    encoder->pix_fmt = AV_PIX_FMT_YUV420P;
    encoder->time_base = {1, 90000};
    encoder->framerate = {30, 1};
    encoder->gop_size = 6;
    encoder->max_b_frames = bframes;
    encoder->thread_count = 1;
    AVDictionary* options = nullptr;
    av_dict_set(&options, "preset", "ultrafast", 0);
    av_dict_set(&options, "x264-params", "annexb=1:repeat-headers=1:scenecut=0:b-adapt=0", 0);
    const int opened = avcodec_open2(encoder.get(), codec, &options);
    av_dict_free(&options);
    Require(opened >= 0);
    frame->format = encoder->pix_fmt;
    frame->width = width;
    frame->height = height;
    frame->sample_aspect_ratio = {1, 1};
    Require(av_frame_get_buffer(frame.get(), 32) >= 0);
    std::vector<media::EncodedFrame> result;
    auto drain = [&] {
        for (;;) {
            const int ret = avcodec_receive_packet(encoder.get(), packet.get());
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            Require(ret >= 0);
            media::EncodedFrame encoded;
            encoded.info.media_type = media::MediaType::Video;
            encoded.info.codec = media::CodecType::H264;
            encoded.info.track_id = 42;
            encoded.info.timestamp.time_base_num = 1;
            encoded.info.timestamp.time_base_den = 90000;
            encoded.info.timestamp.pts = rtp32 ? uint32_t(packet->pts) : packet->pts;
            // Reproduce the depacketizer's DTS=PTS fallback, even with B frames.
            encoded.info.timestamp.dts = encoded.info.timestamp.pts;
            encoded.rtp.ssrc = 99;
            encoded.rtp.packet_count = 1;
            encoded.frame_type = packet->flags & AV_PKT_FLAG_KEY
                ? media::EncodedFrameType::Key : media::EncodedFrameType::Delta;
            encoded.buffer = common::SharedBuffer::Copy(packet->data, packet->size);
            result.push_back(std::move(encoded));
            av_packet_unref(packet.get());
        }
    };
    for (int i = 0; i < count; ++i) {
        Require(av_frame_make_writable(frame.get()) >= 0);
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
                frame->data[0][y * frame->linesize[0] + x] = 70 + ((x / 16 + y / 16 + i) % 80);
        for (int plane = 1; plane <= 2; ++plane)
            for (int y = 0; y < height / 2; ++y)
                std::memset(frame->data[plane] + y * frame->linesize[plane], 128, width / 2);
        frame->pts = origin + i * 3000;
        Require(avcodec_send_frame(encoder.get(), frame.get()) >= 0);
        drain();
    }
    Require(avcodec_send_frame(encoder.get(), nullptr) >= 0);
    drain();
    Require(result.size() == size_t(count));
    return result;
}

struct DecodedResult
{
    std::vector<int64_t> pts;
    int first_top_luma = -1;
    int first_center_luma = -1;
};

// Decode the REAL output bitstream rather than trusting its metadata.
DecodedResult Decode(const std::vector<media::EncodedFrame::ConstPtr>& frames, int width, int height)
{
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    CodecPtr decoder(avcodec_alloc_context3(codec));
    PacketPtr packet(av_packet_alloc());
    FramePtr frame(av_frame_alloc());
    Require(decoder && packet && frame);
    decoder->thread_count = 1;
    decoder->pkt_timebase = {1, 90000};
    decoder->err_recognition = AV_EF_EXPLODE;
    Require(avcodec_open2(decoder.get(), codec, nullptr) >= 0);
    DecodedResult result;
    auto drain = [&] {
        for (;;) {
            const int ret = avcodec_receive_frame(decoder.get(), frame.get());
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            Require(ret >= 0);
            EXPECT_EQ(frame->width, width);
            EXPECT_EQ(frame->height, height);
            EXPECT_EQ(frame->decode_error_flags, 0);
            EXPECT_EQ(frame->flags & AV_FRAME_FLAG_CORRUPT, 0);
            if (result.pts.empty()) {
                result.first_top_luma = frame->data[0][width / 2];
                result.first_center_luma = frame->data[0][height / 2 * frame->linesize[0] + width / 2];
            }
            result.pts.push_back(frame->pts);
            av_frame_unref(frame.get());
        }
    };
    for (const auto& encoded : frames) {
        Require(av_new_packet(packet.get(), static_cast<int>(encoded->Size())) >= 0);
        std::memcpy(packet->data, encoded->Data(), encoded->Size());
        packet->pts = encoded->info.timestamp.pts;
        packet->dts = encoded->info.timestamp.dts;
        Require(avcodec_send_packet(decoder.get(), packet.get()) >= 0);
        av_packet_unref(packet.get());
        drain();
    }
    Require(avcodec_send_packet(decoder.get(), nullptr) >= 0);
    drain();
    return result;
}

void Verify(const std::vector<TranscodedFrame>& outputs, int count, int64_t origin,
            bool letterbox = false, int64_t interval = 3000, int keyframe_interval = 60)
{
    std::map<std::string, std::vector<media::EncodedFrame::ConstPtr>> groups;
    for (const auto& out : outputs) {
        ASSERT_TRUE(out.frame);
        ASSERT_TRUE(out.frame->Valid());
        EXPECT_TRUE(out.frame->IsComplete());
        EXPECT_EQ(out.frame->info.codec, media::CodecType::H264);
        EXPECT_EQ(out.frame->info.media_type, media::MediaType::Video);
        EXPECT_EQ(out.frame->info.timestamp.time_base_num, 1);
        EXPECT_EQ(out.frame->info.timestamp.time_base_den, 90000);
        EXPECT_EQ(out.frame->info.timestamp.pts, out.frame->info.timestamp.dts);
        EXPECT_EQ(out.frame->rtp.ssrc, 0u);
        EXPECT_EQ(out.frame->rtp.packet_count, 0u);
        groups[out.rendition_id].push_back(out.frame);
    }
    ASSERT_EQ(groups.size(), 2u);
    for (const auto& id : {std::string("720p"), std::string("360p")}) {
        const auto& frames = groups.at(id);
        ASSERT_EQ(frames.size(), size_t(count));
        const int width = id == "720p" ? 1280 : 640;
        const int height = id == "720p" ? 720 : 360;
        for (int i = 0; i < count; ++i) {
            EXPECT_EQ(frames[i]->info.track_id, id == "720p" ? 1u : 2u);
            EXPECT_EQ(frames[i]->video.width, width);
            EXPECT_EQ(frames[i]->video.height, height);
            EXPECT_EQ(frames[i]->info.timestamp.pts, origin + i * interval);
            if (i % keyframe_interval == 0) EXPECT_TRUE(frames[i]->IsKeyFrame());
            if (frames[i]->IsKeyFrame()) {
                EXPECT_TRUE(frames[i]->video.is_idr);
                EXPECT_TRUE(frames[i]->video.has_sps);
                EXPECT_TRUE(frames[i]->video.has_pps);
            }
        }
        const auto decoded = Decode(frames, width, height);
        ASSERT_EQ(decoded.pts.size(), size_t(count));
        for (int i = 0; i < count; ++i) EXPECT_EQ(decoded.pts[i], origin + i * interval);
        if (letterbox) {
            EXPECT_NEAR(decoded.first_top_luma, 16, 3);
            EXPECT_GT(decoded.first_center_luma, 50);
        }
        // Each IDR carries the parameters needed for a fresh subscriber.
        for (int i = keyframe_interval; i < count; i += keyframe_interval) {
            const std::vector<media::EncodedFrame::ConstPtr> tail(frames.begin() + i, frames.end());
            EXPECT_EQ(Decode(tail, width, height).pts.size(), size_t(count - i));
        }
    }
}

TEST(VideoTranscoder, Decodes1080pOnceAndProducesTwoIndependentlyDecodableRenditions)
{
    const auto input = MakeInput(1920, 1080, 12);
    VideoTranscodeConfig config;
    config.keyframe_interval = 6;
    VideoTranscoder transcoder;
    ASSERT_TRUE(transcoder.Open(config)) << transcoder.LastError();
    std::vector<TranscodedFrame> output;
    for (const auto& frame : input)
        ASSERT_TRUE(transcoder.Input(frame, output)) << transcoder.LastError();
    ASSERT_TRUE(transcoder.Flush(output)) << transcoder.LastError();
    const auto stats = transcoder.Stats();
    EXPECT_EQ(stats.input_access_units, 12u);
    EXPECT_EQ(stats.decoded_frames, 12u);
    EXPECT_EQ(stats.encoded_frames, 24u);
    Verify(output, 12, 9000, false, 3000, 6);
    const auto size = output.size();
    ASSERT_TRUE(transcoder.Flush(output));
    EXPECT_EQ(output.size(), size);
    EXPECT_FALSE(transcoder.Input(input.front(), output));
    // Outputs own their storage even after every FFmpeg context is freed.
    transcoder.Close();
    Verify(output, 12, 9000, false, 3000, 6);
}

TEST(VideoTranscoder, ReordersBFramesAndDrainsDelayedPictures)
{
    const auto input = MakeInput(640, 360, 12, 2);
    ASSERT_FALSE(std::is_sorted(input.begin(), input.end(), [](const auto& a, const auto& b) {
        return a.info.timestamp.pts < b.info.timestamp.pts;
    }));
    VideoTranscoder transcoder;
    ASSERT_TRUE(transcoder.Open());
    std::vector<TranscodedFrame> output;
    for (const auto& frame : input)
        ASSERT_TRUE(transcoder.Input(frame, output)) << transcoder.LastError();
    EXPECT_LT(transcoder.Stats().decoded_frames, 12u);
    ASSERT_TRUE(transcoder.Flush(output)) << transcoder.LastError();
    Verify(output, 12, 9000);
}

TEST(VideoTranscoder, AcceptsSeparateParameterSetConfigBeforeFirstIdr)
{
    auto input = MakeInput(320, 180, 3);
    const auto bytes = input.front().buffer.ToVector();
    auto prefix = [&](size_t i) -> size_t {
        if (bytes.size() - i >= 3 && !bytes[i] && !bytes[i + 1]) {
            if (bytes[i + 2] == 1) return 3;
            if (bytes.size() - i >= 4 && !bytes[i + 2] && bytes[i + 3] == 1) return 4;
        }
        return 0;
    };
    std::vector<uint8_t> parameters, picture;
    for (size_t i = 0; i < bytes.size();) {
        const size_t start = i;
        const auto size = prefix(i);
        ASSERT_NE(size, 0u);
        i += size;
        ASSERT_LT(i, bytes.size());
        const auto type = bytes[i] & 31;
        while (i < bytes.size() && !prefix(i)) ++i;
        auto& target = type == 7 || type == 8 ? parameters : picture;
        target.insert(target.end(), bytes.begin() + start, bytes.begin() + i);
    }
    ASSERT_FALSE(parameters.empty());
    ASSERT_FALSE(picture.empty());
    auto config_frame = input.front();
    config_frame.frame_type = media::EncodedFrameType::Config;
    config_frame.buffer = common::SharedBuffer::Take(std::move(parameters));
    input.front().buffer = common::SharedBuffer::Take(std::move(picture));
    VideoTranscoder transcoder;
    ASSERT_TRUE(transcoder.Open());
    std::vector<TranscodedFrame> output;
    ASSERT_TRUE(transcoder.Input(config_frame, output)) << transcoder.LastError();
    EXPECT_TRUE(output.empty());
    for (const auto& frame : input)
        ASSERT_TRUE(transcoder.Input(frame, output)) << transcoder.LastError();
    ASSERT_TRUE(transcoder.Flush(output)) << transcoder.LastError();
    EXPECT_EQ(transcoder.Stats().decoded_frames, 3u);
    Verify(output, 3, 9000);
}

TEST(VideoTranscoder, UnwrapsRtpTimestampAcrossWrapWithBFrameReordering)
{
    const int64_t origin = int64_t(UINT32_MAX) - 4500;
    const auto input = MakeInput(320, 180, 8, 2, origin, true);
    VideoTranscodeConfig config;
    config.timestamp_mode = InputTimestampMode::Rtp32;
    VideoTranscoder transcoder;
    ASSERT_TRUE(transcoder.Open(config));
    std::vector<TranscodedFrame> output;
    for (const auto& frame : input)
        ASSERT_TRUE(transcoder.Input(frame, output)) << transcoder.LastError();
    ASSERT_TRUE(transcoder.Flush(output)) << transcoder.LastError();
    Verify(output, 8, origin);
}

TEST(VideoTranscoder, LetterboxesWideInputWithoutStretchingAndConvertsTimeBase)
{
    auto input = MakeInput(640, 180, 4);
    for (size_t i = 0; i < input.size(); ++i) {
        input[i].info.timestamp.pts = 100 + int64_t(i) * 40;
        input[i].info.timestamp.dts = input[i].info.timestamp.pts;
        input[i].info.timestamp.time_base_den = 1000;
    }
    VideoTranscoder transcoder;
    ASSERT_TRUE(transcoder.Open());
    std::vector<TranscodedFrame> output;
    for (const auto& frame : input)
        ASSERT_TRUE(transcoder.Input(frame, output)) << transcoder.LastError();
    ASSERT_TRUE(transcoder.Flush(output)) << transcoder.LastError();
    Verify(output, 4, 9000, true, 3600);
}

TEST(VideoTranscoder, CanReopenWithAnIndependentStreamAndAdaptInputDimensions)
{
    VideoTranscoder transcoder;
    std::vector<TranscodedFrame> output;
    for (const auto dims : {std::pair<int, int>{320, 180}, {960, 540}}) {
        ASSERT_TRUE(transcoder.Open());
        for (const auto& frame : MakeInput(dims.first, dims.second, 3))
            ASSERT_TRUE(transcoder.Input(frame, output)) << transcoder.LastError();
        ASSERT_TRUE(transcoder.Flush(output)) << transcoder.LastError();
        Verify(output, 3, 9000);
        output.clear();
    }
    // Same session, new SPS/PPS and an IDR with a new resolution.
    ASSERT_TRUE(transcoder.Open());
    for (const auto& frame : MakeInput(320, 180, 3)) ASSERT_TRUE(transcoder.Input(frame, output));
    for (const auto& frame : MakeInput(640, 360, 3, 0, 18000))
        ASSERT_TRUE(transcoder.Input(frame, output)) << transcoder.LastError();
    ASSERT_TRUE(transcoder.Flush(output)) << transcoder.LastError();
    Verify(output, 6, 9000);
}

TEST(VideoTranscoder, RejectsInvalidConfigurationAndFramesAndCanRecoverByReopening)
{
    VideoTranscoder transcoder;
    VideoTranscodeConfig config;
    config.renditions[0].encoding.width = 1279;
    EXPECT_FALSE(transcoder.Open(config));
    EXPECT_FALSE(transcoder.LastError().empty());
    config = {};
    config.renditions[1].id = config.renditions[0].id;
    EXPECT_FALSE(transcoder.Open(config));
    config = {};
    config.renditions[1].track_id = config.renditions[0].track_id;
    EXPECT_FALSE(transcoder.Open(config));
    config = {};
    config.renditions.clear();
    EXPECT_FALSE(transcoder.Open(config));
    auto input = MakeInput(320, 180, 2);
    std::vector<TranscodedFrame> output;
    for (int invalid = 0; invalid < 5; ++invalid) {
        ASSERT_TRUE(transcoder.Open());
        auto frame = input.front();
        if (invalid == 0) frame.info.integrity = media::FrameIntegrity::Corrupted;
        if (invalid == 1) frame.info.codec = media::CodecType::H265;
        if (invalid == 2) frame.info.timestamp.time_base_den = 0;
        if (invalid == 3) frame.buffer = common::SharedBuffer::Take({1, 2, 3, 4});
        if (invalid == 4) frame.info.timestamp.pts = AV_NOPTS_VALUE;
        EXPECT_FALSE(transcoder.Input(frame, output));
        EXPECT_FALSE(transcoder.Flush(output));
    }
    ASSERT_TRUE(transcoder.Open());
    for (const auto& frame : input) ASSERT_TRUE(transcoder.Input(frame, output));
    ASSERT_TRUE(transcoder.Flush(output));
    Verify(output, 2, 9000);
}

TEST(VideoTranscoder, FlushesEmptyStreamWithoutProducingPictures)
{
    VideoTranscoder transcoder;
    ASSERT_TRUE(transcoder.Open());
    std::vector<TranscodedFrame> output;
    ASSERT_TRUE(transcoder.Flush(output)) << transcoder.LastError();
    EXPECT_TRUE(output.empty());
    EXPECT_EQ(transcoder.Stats().decoded_frames, 0u);
}

TEST(VideoCodecAdapters, SelectsOnlyRegisteredImplementationsWithoutCrossCodecFallback)
{
    namespace codec = media::codec;
    codec::CodecError error;
    codec::VideoEncoderConfig config;
    config.width = 320;
    config.height = 180;
    config.bitrate_bps = 300000;
    EXPECT_FALSE(codec::VideoCodecFactory::CreateEncoder(config,
        {codec::SelectionMode::Require, "h264_videotoolbox"}, error));
    EXPECT_EQ(error.code, codec::CodecErrorCode::Unsupported);
    EXPECT_NE(error.message.find("h264_videotoolbox"), std::string::npos);
    EXPECT_FALSE(codec::VideoCodecFactory::CreateEncoder(config,
        {codec::SelectionMode::Require, ""}, error));
    EXPECT_EQ(error.code, codec::CodecErrorCode::InvalidConfiguration);
    auto encoder = codec::VideoCodecFactory::CreateEncoder(config,
        {codec::SelectionMode::Prefer, "not-registered"}, error);
    ASSERT_TRUE(encoder) << error.message;
    EXPECT_STREQ(encoder->Implementation(), "libx264");
    EXPECT_EQ(error.code, codec::CodecErrorCode::None);
    config.codec = media::CodecType::AAC;
    EXPECT_FALSE(codec::VideoCodecFactory::CreateEncoder(config, {}, error));
    EXPECT_EQ(error.code, codec::CodecErrorCode::Unsupported);
    EXPECT_FALSE(codec::VideoCodecFactory::CreateDecoder({media::CodecType::AAC}, {}, error));
    EXPECT_EQ(error.code, codec::CodecErrorCode::Unsupported);
    config.codec = media::CodecType::H264;
    config.width = 319;
    EXPECT_FALSE(codec::VideoCodecFactory::CreateEncoder(config, {}, error));
    EXPECT_EQ(error.code, codec::CodecErrorCode::InvalidConfiguration);
    EXPECT_FALSE(codec::VideoCodecFactory::CreateDecoder({},
        {codec::SelectionMode::Require, "not-registered"}, error));
    auto decoder = codec::VideoCodecFactory::CreateDecoder({},
        {codec::SelectionMode::Require, "h264"}, error);
    ASSERT_TRUE(decoder) << error.message;
    EXPECT_STREQ(decoder->Implementation(), "h264");
    const auto capabilities = codec::VideoCodecFactory::Capabilities();
    ASSERT_GE(capabilities.size(), 10u);
    for (const auto& capability : capabilities) {
        EXPECT_TRUE(codec::IsVideoCodec(capability.codec));
        EXPECT_EQ(capability.backend, "ffmpeg");
        EXPECT_TRUE(capability.built_in);
        if (capability.codec == media::CodecType::H264) EXPECT_TRUE(capability.library_available);
    }
}

TEST(VideoCodecAdapters, RawFramesSurviveDecoderAndScalerReuseAndRelease)
{
    namespace codec = media::codec;
    codec::CodecError error;
    auto decoder = codec::VideoCodecFactory::CreateDecoder({}, {}, error);
    ASSERT_TRUE(decoder) << error.message;
    std::vector<codec::RawVideoFrame> raw;
    const auto inputs = MakeInput(320, 180, 3);
    ASSERT_TRUE(decoder->Decode(inputs[0], raw, error)) << error.message;
    ASSERT_EQ(raw.size(), 1u);
    const auto first_luma = raw.front().planes[0][0];
    for (size_t i = 1; i < inputs.size(); ++i)
        ASSERT_TRUE(decoder->Decode(inputs[i], raw, error)) << error.message;
    ASSERT_TRUE(decoder->Flush(raw, error)) << error.message;
    ASSERT_TRUE(decoder->Flush(raw, error));
    ASSERT_EQ(raw.size(), 3u);
    decoder->Close();
    decoder.reset();
    EXPECT_EQ(raw.front().planes[0][0], first_luma);
    EXPECT_NE(raw.front().planes[0][0], raw.back().planes[0][0]);
    auto scaler = codec::VideoCodecFactory::CreateScaler(640, 360, error);
    ASSERT_TRUE(scaler) << error.message;
    std::vector<codec::RawVideoFrame> scaled(3);
    for (size_t i = 0; i < raw.size(); ++i)
        ASSERT_TRUE(scaler->Scale(raw[i], scaled[i], error)) << error.message;
    raw.clear();
    scaler.reset();
    EXPECT_NE(scaled.front().planes[0][0], scaled.back().planes[0][0]);
    codec::VideoEncoderConfig config;
    config.width = 640; config.height = 360; config.bitrate_bps = 600000;
    auto encoder = codec::VideoCodecFactory::CreateEncoder(config, {}, error);
    ASSERT_TRUE(encoder) << error.message;
    std::vector<media::EncodedFrame::Ptr> encoded;
    for (const auto& frame : scaled)
        ASSERT_TRUE(encoder->Encode(frame, encoded, error)) << error.message;
    ASSERT_TRUE(encoder->Flush(encoded, error)) << error.message;
    ASSERT_TRUE(encoder->Flush(encoded, error));
    ASSERT_EQ(encoded.size(), 3u);
    encoder->Close();
    encoder.reset();
    scaled.clear();
    std::vector<media::EncodedFrame::ConstPtr> output(encoded.begin(), encoded.end());
    EXPECT_EQ(Decode(output, 640, 360).pts, (std::vector<int64_t>{9000, 12000, 15000}));
    EXPECT_EQ(output.front()->info.track_id, 0u); // Identity is assigned above the adapter.
    EXPECT_TRUE(output.front()->video.has_sps);
    EXPECT_TRUE(output.front()->video.has_pps);
}

TEST(VideoCodecAdapters, EncodesCpuStorageProvidedOutsideFfmpeg)
{
    namespace codec = media::codec;
    struct CpuStorage final : codec::VideoFrameStorage { std::vector<uint8_t> bytes; };
    auto storage = std::make_shared<CpuStorage>();
    storage->bytes.resize(320 * 180 * 3 / 2, 128);
    std::fill_n(storage->bytes.data(), 320 * 180, 80);
    codec::RawVideoFrame raw;
    raw.storage = storage;
    raw.width = 320; raw.height = 180;
    raw.planes = {storage->bytes.data(), storage->bytes.data() + 320 * 180,
                  storage->bytes.data() + 320 * 180 * 5 / 4, nullptr};
    raw.strides = {320, 160, 160, 0};
    raw.timestamp.pts = 100; // Public CPU inputs may use a different time base.
    codec::VideoEncoderConfig config;
    config.width = 320; config.height = 180; config.bitrate_bps = 300000;
    codec::CodecError error;
    auto encoder = codec::VideoCodecFactory::CreateEncoder(config, {}, error);
    ASSERT_TRUE(encoder) << error.message;
    std::vector<media::EncodedFrame::Ptr> encoded;
    ASSERT_TRUE(encoder->Encode(raw, encoded, error)) << error.message;
    std::fill(storage->bytes.begin(), storage->bytes.end(), 0);
    raw = {};
    storage.reset();
    ASSERT_TRUE(encoder->Flush(encoded, error)) << error.message;
    encoder.reset();
    ASSERT_EQ(encoded.size(), 1u);
    std::vector<media::EncodedFrame::ConstPtr> output(encoded.begin(), encoded.end());
    const auto decoded = Decode(output, 320, 180);
    EXPECT_EQ(decoded.pts, (std::vector<int64_t>{9000}));
    EXPECT_NEAR(decoded.first_center_luma, 80, 3);
}

class FrameCollector final : public media::IEncodedFrameSink
{
public:
    bool SubmitFrame(const media::EncodedFrameEvent& event) override
    {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        changed.notify_all();
        changed.wait(lock, [&] { return !blocked; });
        frames.push_back(event);
        threads.push_back(std::this_thread::get_id());
        changed.notify_all();
        return true;
    }
    bool WaitForEntry()
    {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(5), [&] { return entered; });
    }
    void Release()
    {
        std::lock_guard<std::mutex> lock(mutex);
        blocked = false;
        changed.notify_all();
    }
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, blocked = false;
    std::vector<media::EncodedFrameEvent> frames;
    std::vector<std::thread::id> threads;
};

TranscodeTaskConfig TaskConfig()
{
    TranscodeTaskConfig config;
    config.task_id = "camera-transcode";
    config.input.endpoint_id = 10;
    config.input.track_id = 42;
    config.input.ssrc = 99;
    config.output.endpoint_id = 20;
    config.output.session_id = "room";
    config.output.stream_id = "camera";
    return config;
}

media::EncodedFrameEvent Event(const media::EncodedFrame& frame)
{
    media::EncodedFrameEvent event;
    event.source.endpoint_id = 10;
    event.source.track_id = frame.info.track_id;
    event.source.ssrc = 99;
    event.frame = std::make_shared<const media::EncodedFrame>(frame);
    return event;
}

std::vector<TranscodedFrame> CollectedFrames(const FrameCollector& sink)
{
    // Call only after StopTask has joined the publishing worker.
    std::vector<TranscodedFrame> output;
    for (const auto& event : sink.frames) {
        EXPECT_EQ(event.source.endpoint_id, 20u);
        EXPECT_EQ(event.source.session_id, "room");
        EXPECT_EQ(event.source.track_id, event.frame->info.track_id);
        const auto rendition = event.source.stream_id.substr(event.source.stream_id.find('/') + 1);
        EXPECT_EQ(event.source.stream_id, "camera/" + rendition);
        output.push_back({rendition, event.frame});
    }
    return output;
}

TEST(TranscodeService, RoutesOnlyRequestedTrackOnWorkerAndDrainsOnStop)
{
    auto input = std::make_shared<media::EncodedFrameRouter>();
    auto output = std::make_shared<media::EncodedFrameRouter>();
    auto sink = std::make_shared<FrameCollector>();
    output->Subscribe(sink);
    TranscodeService service(input, output);
    ASSERT_TRUE(service.Init());
    ASSERT_TRUE(service.Start());
    ASSERT_TRUE(service.StartTask(TaskConfig())) << service.LastError();
    auto frames = MakeInput(640, 360, 10, 2);
    auto wrong = Event(frames.front());
    wrong.source.endpoint_id = 999;
    EXPECT_EQ(input->Publish(wrong), 0u);
    wrong = Event(frames.front());
    wrong.source.track_id = 1;
    EXPECT_EQ(input->Publish(wrong), 0u);
    for (const auto& frame : frames) EXPECT_EQ(input->Publish(Event(frame)), 1u);
    TranscodeTaskStatus final;
    ASSERT_TRUE(service.StopTask(TaskConfig().task_id, &final));
    EXPECT_EQ(final.state, TranscodeTaskState::Stopped) << final.last_error;
    EXPECT_EQ(final.stats.accepted, 10u);
    EXPECT_EQ(final.stats.decoded_frames, 10u);
    EXPECT_EQ(final.stats.encoded_frames, 20u);
    EXPECT_EQ(final.stats.published, 20u);
    EXPECT_EQ(final.stats.errors, 0u);
    EXPECT_EQ(final.stats.queue_bytes, 0u);
    for (auto thread : sink->threads) EXPECT_NE(thread, std::this_thread::get_id());
    Verify(CollectedFrames(*sink), 10, 9000);
    EXPECT_EQ(input->Publish(Event(frames.front())), 0u);
    EXPECT_FALSE(service.TaskStatus(TaskConfig().task_id));
}

struct BackendTrace
{
    std::vector<std::string> calls;
    std::vector<std::thread::id> threads;
    void Record(const std::string& call)
    {
        calls.push_back(call);
        threads.push_back(std::this_thread::get_id());
    }
};

class ProbeBackend final : public IVideoTranscoder
{
public:
    explicit ProbeBackend(std::shared_ptr<BackendTrace> trace, bool fail = false)
        : trace_(std::move(trace)), fail_(fail) { trace_->Record("create"); }
    ~ProbeBackend() override { Close(); trace_->Record("destroy"); }
    bool Open(const VideoTranscodeConfig&) override { trace_->Record("open"); return !fail_; }
    bool Input(const media::EncodedFrame&, std::vector<TranscodedFrame>&) override
    {
        trace_->Record("input");
        ++stats_.decoded_frames;
        return true;
    }
    bool Flush(std::vector<TranscodedFrame>&) override { trace_->Record("flush"); return true; }
    void Close() noexcept override { trace_->Record("close"); }
    const std::string& LastError() const noexcept override { return error_; }
    VideoTranscodeStats Stats() const noexcept override { return stats_; }
private:
    std::shared_ptr<BackendTrace> trace_;
    bool fail_;
    VideoTranscodeStats stats_;
    const std::string error_ = "probe backend failed to open";
};

TEST(TranscodeService, InjectedBackendLivesEntirelyOnTaskWorker)
{
    auto input = std::make_shared<media::EncodedFrameRouter>();
    auto output = std::make_shared<media::EncodedFrameRouter>();
    auto trace = std::make_shared<BackendTrace>();
    TranscodeService service(input, output, 1,
        [trace] { return std::make_unique<ProbeBackend>(trace); });
    ASSERT_TRUE(service.Init());
    ASSERT_TRUE(service.Start());
    auto config = TaskConfig();
    ASSERT_TRUE(service.StartTask(config)) << service.LastError();
    // Modifying the caller's config must not redirect an admitted task.
    config.input.endpoint_id = 999;
    const auto frames = MakeInput(320, 180, 1);
    EXPECT_EQ(input->Publish(Event(frames.front())), 1u);
    TranscodeTaskStatus final;
    ASSERT_TRUE(service.StopTask(config.task_id, &final));
    EXPECT_EQ(final.stats.processed, 1u);
    EXPECT_EQ(final.stats.decoded_frames, 1u);
    EXPECT_EQ(trace->calls, (std::vector<std::string>{"create", "open", "input", "flush", "close", "destroy"}));
    ASSERT_FALSE(trace->threads.empty());
    for (const auto thread : trace->threads) {
        EXPECT_EQ(thread, trace->threads.front());
        EXPECT_NE(thread, std::this_thread::get_id());
    }
}

TEST(TranscodeService, BackendStartupFailuresReleaseTaskSubscriptionAndWorker)
{
    auto input = std::make_shared<media::EncodedFrameRouter>();
    auto output = std::make_shared<media::EncodedFrameRouter>();
    const auto frames = MakeInput(320, 180, 1);
    for (int failure = 0; failure != 3; ++failure) {
        SCOPED_TRACE(failure);
        auto trace = std::make_shared<BackendTrace>();
        TranscodeService service(input, output, 1,
            [failure, trace]() -> std::unique_ptr<IVideoTranscoder> {
                if (failure == 0) return nullptr;
                if (failure == 1) throw std::runtime_error("backend unavailable");
                return std::make_unique<ProbeBackend>(trace, true);
            });
        ASSERT_TRUE(service.Init());
        ASSERT_TRUE(service.Start());
        EXPECT_FALSE(service.StartTask(TaskConfig()));
        EXPECT_FALSE(service.LastError().empty());
        EXPECT_FALSE(service.TaskStatus(TaskConfig().task_id));
        EXPECT_EQ(input->Publish(Event(frames.front())), 0u);
        for (const auto& thread : utils::ThreadManager::Instance().Snapshot())
            EXPECT_NE(thread.options.group, "transcode");
        // Failed admission must free the task ID and capacity for a retry.
        EXPECT_FALSE(service.StartTask(TaskConfig()));
        EXPECT_EQ(service.LastError().find("capacity"), std::string::npos);
    }
}

TEST(TranscodeService, QueueOverflowWaitsForIdrAndResetsDecoderWithoutBlockingIngress)
{
    auto input = std::make_shared<media::EncodedFrameRouter>();
    auto output = std::make_shared<media::EncodedFrameRouter>();
    auto sink = std::make_shared<FrameCollector>();
    sink->blocked = true;
    output->Subscribe(sink);
    TranscodeService service(input, output);
    // Release even when an ASSERT exits early, before the service joins.
    struct Release { std::shared_ptr<FrameCollector> sink; ~Release() { sink->Release(); } } release{sink};
    ASSERT_TRUE(service.Init());
    ASSERT_TRUE(service.Start());
    auto config = TaskConfig();
    config.queue.max_frames = 1;
    std::atomic<int> requests{0};
    ASSERT_TRUE(service.StartTask(config, [&] { ++requests; }));
    const auto frames = MakeInput(320, 180, 8);
    EXPECT_EQ(input->Publish(Event(frames[0])), 1u);
    ASSERT_TRUE(sink->WaitForEntry()); // Worker is held inside its first output.
    EXPECT_EQ(input->Publish(Event(frames[1])), 1u);
    EXPECT_EQ(input->Publish(Event(frames[2])), 0u); // Clears the dependent backlog.
    EXPECT_EQ(input->Publish(Event(frames[3])), 0u); // Delta cannot resume decoding.
    EXPECT_TRUE(service.TaskStatus(config.task_id)->waiting_for_idr);
    EXPECT_EQ(input->Publish(Event(frames[6])), 1u); // Next independently decodable IDR.
    sink->Release();
    TranscodeTaskStatus final;
    ASSERT_TRUE(service.StopTask(config.task_id, &final));
    EXPECT_EQ(final.state, TranscodeTaskState::Stopped) << final.last_error;
    EXPECT_GE(final.stats.recoveries, 1u);
    EXPECT_EQ(final.stats.decoded_frames, 2u);
    EXPECT_EQ(final.stats.encoded_frames, 4u);
    EXPECT_GE(final.stats.dropped, 3u);
    EXPECT_GE(requests.load(), 1);
    const auto outputs = CollectedFrames(*sink);
    for (const auto& id : {"720p", "360p"}) {
        std::vector<media::EncodedFrame::ConstPtr> rendition;
        for (const auto& frame : outputs) if (frame.rendition_id == id) rendition.push_back(frame.frame);
        ASSERT_EQ(rendition.size(), 2u);
        EXPECT_TRUE(rendition[1]->IsKeyFrame());
        EXPECT_EQ(Decode(rendition, std::string(id) == "720p" ? 1280 : 640,
                         std::string(id) == "720p" ? 720 : 360).pts.size(), 2u);
    }
}

TEST(TranscodeService, WaitsForIdrAndPreservesRtpEpochAcrossRecovery)
{
    const int64_t origin = int64_t(UINT32_MAX) - 4500;
    const auto frames = MakeInput(320, 180, 8, 0, origin, true);
    auto input = std::make_shared<media::EncodedFrameRouter>();
    auto output = std::make_shared<media::EncodedFrameRouter>();
    auto sink = std::make_shared<FrameCollector>();
    output->Subscribe(sink);
    TranscodeService service(input, output);
    ASSERT_TRUE(service.Init());
    ASSERT_TRUE(service.Start());
    ASSERT_TRUE(service.StartTask(TaskConfig()));
    EXPECT_EQ(input->Publish(Event(frames[1])), 0u); // Cannot start with a delta.
    EXPECT_EQ(input->Publish(Event(frames[0])), 1u);
    ASSERT_TRUE(sink->WaitForEntry());
    auto broken = frames[2];
    broken.info.integrity = media::FrameIntegrity::Corrupted;
    EXPECT_EQ(input->Publish(Event(broken)), 0u);
    EXPECT_EQ(input->Publish(Event(frames[3])), 0u);
    EXPECT_EQ(input->Publish(Event(frames[6])), 1u);
    TranscodeTaskStatus final;
    ASSERT_TRUE(service.StopTask(TaskConfig().task_id, &final));
    EXPECT_EQ(final.state, TranscodeTaskState::Stopped) << final.last_error;
    EXPECT_EQ(final.stats.decoded_frames, 2u);
    EXPECT_GE(final.stats.recoveries, 1u);
    const auto outputs = CollectedFrames(*sink);
    for (const auto& id : {"720p", "360p"}) {
        std::vector<media::EncodedFrame::ConstPtr> rendition;
        for (const auto& frame : outputs) if (frame.rendition_id == id) rendition.push_back(frame.frame);
        ASSERT_EQ(rendition.size(), 2u);
        EXPECT_EQ(rendition[0]->info.timestamp.pts, origin);
        EXPECT_EQ(rendition[1]->info.timestamp.pts, origin + 18000);
        EXPECT_TRUE(rendition[1]->video.has_sps);
        EXPECT_TRUE(rendition[1]->video.has_pps);
    }
}

TEST(TranscodeService, EnforcesTaskLimitAndSourceUniquenessAndStopsAllWorkers)
{
    auto input = std::make_shared<media::EncodedFrameRouter>();
    auto output = std::make_shared<media::EncodedFrameRouter>();
    TranscodeService service(input, output, 2);
    EXPECT_FALSE(service.StartTask(TaskConfig()));
    ASSERT_TRUE(service.Init());
    ASSERT_TRUE(service.Start());
    auto config = TaskConfig();
    ASSERT_TRUE(service.StartTask(config));
    EXPECT_FALSE(service.StartTask(config));
    config.task_id = "duplicate-source";
    config.output.endpoint_id = 21;
    EXPECT_FALSE(service.StartTask(config));
    config.input.endpoint_id = 11;
    config.task_id = "second";
    ASSERT_TRUE(service.StartTask(config));
    config.task_id = "third";
    config.input.endpoint_id = 12;
    config.output.endpoint_id = 22;
    EXPECT_FALSE(service.StartTask(config));
    EXPECT_TRUE(service.Health().healthy);
    service.Stop();
    EXPECT_FALSE(service.TaskStatus("second"));
    const auto frames = MakeInput(320, 180, 1);
    EXPECT_EQ(input->Publish(Event(frames.front())), 0u);
    ASSERT_TRUE(service.Start());
    ASSERT_TRUE(service.StartTask(TaskConfig()));
    service.Stop();
    for (const auto& thread : utils::ThreadManager::Instance().Snapshot())
        EXPECT_NE(thread.options.group, "transcode");
    TranscodeService invalid(input, input);
    EXPECT_FALSE(invalid.Init());
}

#ifdef PACKETIA_TEST_SFU_PIPELINE
std::vector<std::vector<uint8_t>> PacketizeForTest(const media::EncodedFrame& frame, uint16_t& sequence)
{
    const auto bytes = frame.buffer.ToVector();
    auto prefix = [&](size_t i) -> size_t {
        if (bytes.size() - i >= 3 && !bytes[i] && !bytes[i + 1]) {
            if (bytes[i + 2] == 1) return 3;
            if (bytes.size() - i >= 4 && !bytes[i + 2] && bytes[i + 3] == 1) return 4;
        }
        return 0;
    };
    std::vector<std::vector<uint8_t>> payloads;
    for (size_t i = 0; i < bytes.size();) {
        const auto size = prefix(i);
        Require(size != 0);
        i += size;
        const size_t start = i;
        while (i < bytes.size() && !prefix(i)) ++i;
        if (i - start <= 300) payloads.emplace_back(bytes.begin() + start, bytes.begin() + i);
        else {
            for (size_t pos = start + 1; pos < i;) {
                const auto end = std::min(pos + 298, i);
                std::vector<uint8_t> payload{
                    static_cast<uint8_t>((bytes[start] & 0xE0) | 28),
                    static_cast<uint8_t>((bytes[start] & 31) | (pos == start + 1 ? 0x80 : 0) | (end == i ? 0x40 : 0))};
                payload.insert(payload.end(), bytes.begin() + pos, bytes.begin() + end);
                payloads.push_back(std::move(payload));
                pos = end;
            }
        }
    }
    std::vector<std::vector<uint8_t>> packets;
    const auto timestamp = uint32_t(frame.info.timestamp.pts);
    for (size_t i = 0; i < payloads.size(); ++i) {
        std::vector<uint8_t> packet{0x80, static_cast<uint8_t>(96 | (i + 1 == payloads.size() ? 0x80 : 0)),
            static_cast<uint8_t>(sequence >> 8), static_cast<uint8_t>(sequence),
            static_cast<uint8_t>(timestamp >> 24), static_cast<uint8_t>(timestamp >> 16),
            static_cast<uint8_t>(timestamp >> 8), static_cast<uint8_t>(timestamp), 0, 0, 0, 99};
        ++sequence;
        packet.insert(packet.end(), payloads[i].begin(), payloads[i].end());
        packets.push_back(std::move(packet));
    }
    // Exercise actual receiver/FU reassembly with packet reordering.
    for (size_t i = 1; i + 1 < packets.size(); i += 2) std::swap(packets[i], packets[i + 1]);
    return packets;
}

TEST(TranscodeService, RealRtpReceiverReassemblesReorderedFuAndTranscodesThroughFrameRouter)
{
    const auto input_frames = MakeInput(1920, 1080, 12, 2, int64_t(UINT32_MAX) - 4500, true);
    auto input = std::make_shared<media::EncodedFrameRouter>();
    auto output = std::make_shared<media::EncodedFrameRouter>();
    auto sink = std::make_shared<FrameCollector>();
    output->Subscribe(sink);
    TranscodeService service(input, output);
    ASSERT_TRUE(service.Init());
    ASSERT_TRUE(service.Start());
    ASSERT_TRUE(service.StartTask(TaskConfig()));
    auto endpoint = std::make_shared<media::SfuEndpoint>(10, input);
    ::TrackInfo info;
    info.track_index = 42;
    info.type = TrackVideo;
    info.codec_id = CodecId::H264;
    info.codec_name = "H264";
    info.payload_type = 96;
    info.clock_rate = 90000;
    info.ssrc = 99;
    ASSERT_TRUE(endpoint->AddPublishedTrack("video", std::make_shared<RtpTrackDescription>(info)));
    ASSERT_TRUE(endpoint->Start());
    uint16_t sequence = 65530;
    for (const auto& frame : input_frames) {
        for (const auto& packet : PacketizeForTest(frame, sequence)) {
            WorkJob job;
            job.payload = common::SharedBuffer::Copy(packet.data(), packet.size());
            endpoint->OnRtp(job);
        }
    }
    endpoint->Stop();
    TranscodeTaskStatus final;
    ASSERT_TRUE(service.StopTask(TaskConfig().task_id, &final));
    EXPECT_EQ(final.state, TranscodeTaskState::Stopped) << final.last_error;
    EXPECT_EQ(final.stats.accepted, 12u);
    EXPECT_EQ(final.stats.decoded_frames, 12u);
    EXPECT_EQ(final.stats.encoded_frames, 24u);
    EXPECT_EQ(final.stats.errors, 0u);
    Verify(CollectedFrames(*sink), 12, int64_t(UINT32_MAX) - 4500);
}
#endif
} // namespace
