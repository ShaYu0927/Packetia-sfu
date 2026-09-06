#include "RecordService/RecordingService.h"
#include "AudioDepacketizer.h"
#include "H264Depacketizer.h"
#include <gtest/gtest.h>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/version.h>
#include <libavcodec/bsf.h>
#include <libavutil/base64.h>
}
#include <chrono>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <thread>

namespace {
using namespace std::chrono_literals;
using media::EncodedFrameEvent;
std::vector<EncodedFrameEvent> LoadFrames() {
    AVFormatContext* input = nullptr;
    if (avformat_open_input(&input, RECORDING_FIXTURE, nullptr, nullptr) < 0) return {};
    if (avformat_find_stream_info(input, nullptr) < 0) { avformat_close_input(&input); return {}; }
    AVBSFContext* annexb = nullptr;
    std::vector<EncodedFrameEvent> frames;
    AVPacket* packet = av_packet_alloc();
    while (av_read_frame(input, packet) >= 0) {
        const auto* stream = input->streams[packet->stream_index];
        const auto* parameters = stream->codecpar;
        const bool video = parameters->codec_type == AVMEDIA_TYPE_VIDEO;
        auto frame = std::make_shared<media::EncodedFrame>();
        frame->info.media_type = video ? media::MediaType::Video : media::MediaType::Audio;
        frame->info.codec = video ? media::CodecType::H264 : media::CodecType::AAC;
        frame->info.track_id = packet->stream_index;
        const int rate = video ? 90000 : parameters->sample_rate;
        // Distinct random RTP origins, including a wrap in the video timeline.
        const uint32_t origin = video ? 0xFFFF0000U : 1700000000U;
        const int64_t ticks = av_rescale_q(packet->dts, stream->time_base, AVRational{1, rate});
        frame->info.timestamp.dts = frame->info.timestamp.pts = static_cast<uint32_t>(origin + ticks);
        frame->info.timestamp.time_base_den = rate;
        frame->info.timestamp.receive_time_ms = 100000 + av_rescale_q(packet->dts, stream->time_base, AVRational{1, 1000});
        frame->rtp.ssrc = video ? 100 : 200;
        frame->rtp.rtp_timestamp = frame->info.timestamp.pts;
        frame->sample_rate = rate;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 24, 100)
        frame->channels = video ? 0 : parameters->ch_layout.nb_channels;
#else
        frame->channels = video ? 0 : parameters->channels;
#endif
        frame->sample_count = video ? 0 : 1024;
        frame->video.width = parameters->width;
        frame->video.height = parameters->height;
        frame->frame_type = video ? (packet->flags & AV_PKT_FLAG_KEY ? media::EncodedFrameType::Key : media::EncodedFrameType::Delta) : media::EncodedFrameType::Audio;
        if (video) {
            if (!annexb) {
                av_bsf_alloc(av_bsf_get_by_name("h264_mp4toannexb"), &annexb);
                avcodec_parameters_copy(annexb->par_in, parameters);
                annexb->time_base_in = stream->time_base;
                av_bsf_init(annexb);
            }
            av_bsf_send_packet(annexb, packet);
            if (av_bsf_receive_packet(annexb, packet) < 0) continue;
        } else {
            frame->codec_config = std::make_shared<std::vector<uint8_t>>(
                parameters->extradata, parameters->extradata + parameters->extradata_size);
        }
        frame->buffer = std::make_shared<std::vector<uint8_t>>(packet->data, packet->data + packet->size);
        frame->size = packet->size;
        EncodedFrameEvent event;
        event.source = {video ? 1U : 2U, "1", "live/test", frame->info.track_id, frame->rtp.ssrc};
        event.frame = frame;
        frames.push_back(event);
        av_packet_unref(packet);
    }
    av_packet_free(&packet);
    av_bsf_free(&annexb);
    avformat_close_input(&input);
    return frames;
}

class RecordingTest : public testing::Test {
protected:
    std::filesystem::path directory;
    std::shared_ptr<media::EncodedFrameRouter> router = std::make_shared<media::EncodedFrameRouter>();
    service::RecordingOptions options;
    std::vector<EncodedFrameEvent> frames;
    void SetUp() override {
        directory = std::filesystem::temp_directory_path() /
            ("packetia-record-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        options.directory = directory.string();
        options.discovery_ms = 60000;
        frames = LoadFrames();
        ASSERT_GT(frames.size(), 100U);
    }
    void TearDown() override { std::filesystem::remove_all(directory); }
    std::vector<std::filesystem::path> Files() {
        std::vector<std::filesystem::path> result;
        for (const auto& file : std::filesystem::directory_iterator(directory))
            if (file.path().extension() == ".mp4") result.push_back(file.path());
        return result;
    }
    void VerifyDecode(const std::filesystem::path& path, bool audio = true, bool partial = false, size_t expected_video = 50) {
        AVFormatContext* input = nullptr;
        ASSERT_GE(avformat_open_input(&input, path.c_str(), nullptr, nullptr), 0);
        ASSERT_GE(avformat_find_stream_info(input, nullptr), 0);
        EXPECT_EQ(input->nb_streams, (audio ? 1U : 0U) + (expected_video ? 1U : 0U));
        std::vector<AVCodecContext*> decoders(input->nb_streams);
        std::vector<int64_t> last(input->nb_streams, AV_NOPTS_VALUE);
        size_t video_frames = 0, audio_frames = 0;
        for (unsigned i = 0; i < input->nb_streams; ++i) {
            auto* parameters = input->streams[i]->codecpar;
            decoders[i] = avcodec_alloc_context3(avcodec_find_decoder(parameters->codec_id));
            ASSERT_NE(decoders[i], nullptr);
            ASSERT_GE(avcodec_parameters_to_context(decoders[i], parameters), 0);
            ASSERT_GE(avcodec_open2(decoders[i], decoders[i]->codec, nullptr), 0);
        }
        auto* packet = av_packet_alloc();
        auto* frame = av_frame_alloc();
        auto receive = [&](AVCodecContext* decoder) {
            int result;
            while ((result = avcodec_receive_frame(decoder, frame)) >= 0) {
                if (decoder->codec_type == AVMEDIA_TYPE_VIDEO) ++video_frames;
                else ++audio_frames;
                av_frame_unref(frame);
            }
            EXPECT_TRUE(result == AVERROR(EAGAIN) || result == AVERROR_EOF);
        };
        while (av_read_frame(input, packet) >= 0) {
            const auto index = packet->stream_index;
            EXPECT_GE(packet->dts, 0);
            if (last[index] != AV_NOPTS_VALUE) EXPECT_GT(packet->dts, last[index]);
            last[index] = packet->dts;
            EXPECT_GE(avcodec_send_packet(decoders[index], packet), 0);
            receive(decoders[index]);
            av_packet_unref(packet);
        }
        for (auto*& decoder : decoders) {
            EXPECT_GE(avcodec_send_packet(decoder, nullptr), 0);
            receive(decoder);
            avcodec_free_context(&decoder);
        }
        if (partial) EXPECT_GT(video_frames, 0U);
        else EXPECT_EQ(video_frames, expected_video);
        if (audio) EXPECT_GE(audio_frames, partial ? 1U : 80U);
        EXPECT_GE(input->duration, partial ? 1 : 1900000);
        EXPECT_LE(input->duration, 2200000);
        av_frame_free(&frame);
        av_packet_free(&packet);
        avformat_close_input(&input);
    }
};

TEST_F(RecordingTest, RecordsAudioVideoAndUnwrapsRtpTimestamps) {
    auto recorder = std::make_shared<service::RecordingService>(router, options);
    ASSERT_TRUE(recorder->Init());
    ASSERT_TRUE(recorder->Start());
    for (const auto& frame : frames) EXPECT_EQ(router->Publish(frame), 1U);
    recorder->Stop();
    EXPECT_EQ(recorder->Stats().errors, 0U);
    EXPECT_EQ(recorder->Stats().completed_files, 1U);
    auto files = Files();
    ASSERT_EQ(files.size(), 1U);
    VerifyDecode(files.front());
}

TEST_F(RecordingTest, VideoOnlyFinalizesAfterIdleAndCanRestart) {
    options.idle_timeout_ms = 150;
    auto recorder = std::make_shared<service::RecordingService>(router, options);
    ASSERT_TRUE(recorder->Init());
    ASSERT_TRUE(recorder->Start());
    for (const auto& frame : frames)
        if (frame.frame->info.media_type == media::MediaType::Video) EXPECT_EQ(router->Publish(frame), 1U);
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (recorder->Stats().completed_files == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);
    EXPECT_EQ(recorder->Stats().completed_files, 1U);
    recorder->Stop();
    ASSERT_TRUE(recorder->Start());
    for (const auto& frame : frames)
        if (frame.frame->info.media_type == media::MediaType::Video) router->Publish(frame);
    recorder->Stop();
    EXPECT_EQ(recorder->Stats().errors, 0U);
    auto files = Files();
    ASSERT_EQ(files.size(), 2U);
    for (const auto& file : files) VerifyDecode(file, false);
}

TEST_F(RecordingTest, PreservesAudioVideoStartOffset) {
    auto recorder = std::make_shared<service::RecordingService>(router, options);
    ASSERT_TRUE(recorder->Init());
    ASSERT_TRUE(recorder->Start());
    int64_t first_audio_ms = 0, first_video_ms = 0;
    for (auto event : frames) {
        auto frame = std::make_shared<media::EncodedFrame>(*event.frame);
        const bool audio = frame->info.media_type == media::MediaType::Audio;
        if (audio) frame->info.timestamp.receive_time_ms += 250;
        if (audio && !first_audio_ms) first_audio_ms = frame->info.timestamp.receive_time_ms;
        if (!audio && !first_video_ms) first_video_ms = frame->info.timestamp.receive_time_ms;
        event.frame = frame;
        router->Publish(event);
    }
    recorder->Stop();
    ASSERT_EQ(recorder->Stats().errors, 0U);
    const auto files = Files();
    ASSERT_EQ(files.size(), 1U);
    AVFormatContext* input = nullptr;
    ASSERT_GE(avformat_open_input(&input, files.front().c_str(), nullptr, nullptr), 0);
    ASSERT_GE(avformat_find_stream_info(input, nullptr), 0);
    for (unsigned i = 0; i < input->nb_streams; ++i) {
        const auto* stream = input->streams[i];
        if (stream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
        const auto start_ms = av_rescale_q(stream->start_time, stream->time_base, AVRational{1, 1000});
        EXPECT_NEAR(start_ms, first_audio_ms - first_video_ms, 2);
    }
    avformat_close_input(&input);
}

TEST_F(RecordingTest, AudioOnlyIsPlayable) {
    auto recorder = std::make_shared<service::RecordingService>(router, options);
    ASSERT_TRUE(recorder->Init());
    ASSERT_TRUE(recorder->Start());
    for (const auto& event : frames)
        if (event.frame->info.media_type == media::MediaType::Audio) router->Publish(event);
    recorder->Stop();
    EXPECT_EQ(recorder->Stats().errors, 0U);
    const auto files = Files();
    ASSERT_EQ(files.size(), 1U);
    VerifyDecode(files.front(), true, false, 0);
}

TEST_F(RecordingTest, DiscoveryMemoryLimitAndInvalidDirectoryReportFailure) {
    options.max_pending_bytes = 1;
    auto recorder = std::make_shared<service::RecordingService>(router, options);
    ASSERT_TRUE(recorder->Init());
    ASSERT_TRUE(recorder->Start());
    for (const auto& event : frames) router->Publish(event);
    recorder->Stop();
    EXPECT_EQ(recorder->Stats().errors, 1U);
    EXPECT_TRUE(Files().empty());
    options.directory = std::string(RECORDING_FIXTURE) + "/cannot-create-under-a-file";
    recorder = std::make_shared<service::RecordingService>(router, options);
    EXPECT_FALSE(recorder->Init());
    EXPECT_EQ(recorder->State(), service::ServiceState::Failed);
}

TEST_F(RecordingTest, RejectsOversizedQueueInputAndUnsafeStreamNamesStayInDirectory) {
    options.max_queue_bytes = 1;
    auto recorder = std::make_shared<service::RecordingService>(router, options);
    ASSERT_TRUE(recorder->Init());
    ASSERT_TRUE(recorder->Start());
    EXPECT_EQ(router->Publish(frames.front()), 0U);
    recorder->Stop();
    EXPECT_EQ(recorder->Stats().dropped, 1U);
    EXPECT_EQ(recorder->Stats().queue_bytes, 0U);
    EXPECT_TRUE(Files().empty());

    options.max_queue_bytes = 64 * 1024 * 1024;
    recorder = std::make_shared<service::RecordingService>(router, options);
    ASSERT_TRUE(recorder->Init());
    ASSERT_TRUE(recorder->Start());
    for (auto frame : frames) { frame.source.stream_id = "../../evil\\name"; router->Publish(frame); }
    recorder->Stop();
    auto files = Files();
    ASSERT_EQ(files.size(), 1U);
    EXPECT_EQ(files.front().parent_path(), directory);
    VerifyDecode(files.front());
}

TEST_F(RecordingTest, BackwardsTimestampReportsFailure) {
    auto recorder = std::make_shared<service::RecordingService>(router, options);
    ASSERT_TRUE(recorder->Init());
    ASSERT_TRUE(recorder->Start());
    auto first = std::find_if(frames.begin(), frames.end(), [](const auto& event) { return event.frame->IsKeyFrame(); });
    ASSERT_NE(first, frames.end());
    router->Publish(*first);
    auto bad = *first;
    auto frame = std::make_shared<media::EncodedFrame>(*bad.frame);
    frame->info.timestamp.dts -= 100;
    frame->frame_type = media::EncodedFrameType::Delta;
    bad.frame = frame;
    router->Publish(bad);
    recorder->Stop();
    EXPECT_EQ(recorder->Stats().errors, 1U);
}

TEST_F(RecordingTest, SeparatesStreamsAndSplitsOnlyAtVideoKeyframes) {
    options.segment_ms = 80;
    options.discovery_ms = 0;
    auto recorder = std::make_shared<service::RecordingService>(router, options);
    ASSERT_TRUE(recorder->Init());
    ASSERT_TRUE(recorder->Start());
    for (const auto& frame : frames) {
        if (frame.frame->info.media_type != media::MediaType::Video) continue;
        EXPECT_EQ(router->Publish(frame), 1U);
        auto second = frame;
        second.source.session_id = "2";
        second.source.stream_id = "other";
        EXPECT_EQ(router->Publish(second), 1U);
        std::this_thread::sleep_for(5ms);
    }
    recorder->Stop();
    EXPECT_EQ(recorder->Stats().errors, 0U);
    const auto files = Files();
    ASSERT_EQ(files.size(), 4U);
    for (const auto& file : files) VerifyDecode(file, false, true);
}

TEST_F(RecordingTest, SeedsH264ParameterSetsFromSdp) {
    auto first = std::find_if(frames.begin(), frames.end(), [](const auto& event) { return event.frame->IsKeyFrame(); });
    ASSERT_NE(first, frames.end());
    const auto* data = first->frame->Data();
    const auto size = first->frame->size;
    std::string sprop;
    for (size_t pos = 0; pos + 4 < size; ++pos) {
        if (data[pos] || data[pos+1] || data[pos+2] || data[pos+3] != 1) continue;
        const auto start = pos + 4;
        const auto type = data[start] & 31;
        if (type != 7 && type != 8) continue;
        size_t end = start;
        while (end + 3 < size && !(data[end] == 0 && data[end+1] == 0 &&
            (data[end+2] == 1 || (data[end+2] == 0 && data[end+3] == 1)))) ++end;
        std::string encoded(AV_BASE64_SIZE(end - start), '\0');
        ASSERT_NE(av_base64_encode(encoded.data(), encoded.size(), data + start, end - start), nullptr);
        encoded.resize(std::strlen(encoded.c_str()));
        if (!sprop.empty()) sprop += ',';
        sprop += encoded;
    }
    ASSERT_FALSE(sprop.empty());
    H264Depacketizer depacketizer("packetization-mode=1; sprop-parameter-sets=" + sprop);
    const auto* sps = depacketizer.parameterSets().LatestSps();
    ASSERT_NE(sps, nullptr);
    EXPECT_EQ(sps->width, 160);
    EXPECT_EQ(sps->height, 120);
    ASSERT_NE(depacketizer.parameterSets().LatestPps(), nullptr);
    H264Depacketizer invalid("sprop-parameter-sets=%%%%,a");
    EXPECT_EQ(invalid.parameterSets().LatestSps(), nullptr);
}

TEST(RecordingParameters, PreservesAacAudioSpecificConfigFromSdp) {
    media::AudioDepacketizer depacketizer(media::CodecType::AAC, 44100, 2, "config=1210;sizeLength=13;indexLength=3");
    uint8_t payload[] = {0, 16, 0, 16, 0x21, 0x10};
    RtpView view{};
    view.payload = payload;
    view.payload_len = sizeof(payload);
    view.ssrc = 1;
    ASSERT_TRUE(depacketizer.Input(view));
    media::EncodedFrame frame;
    ASSERT_TRUE(depacketizer.PopFrame(frame));
    ASSERT_NE(frame.codec_config, nullptr);
    EXPECT_EQ(*frame.codec_config, (std::vector<uint8_t>{0x12, 0x10}));
}
}
