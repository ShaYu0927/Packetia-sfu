#pragma once
#include "Mp4Recorder.h"
#include "LocalFileIO.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/bsf.h>
#include <libavcodec/version.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>

#define CHECK(expr) do { if (!(expr)) throw std::runtime_error(#expr); } while (false)

namespace recording_test {
struct TimedFrame {
    media::EncodedFrameEvent event;
    int64_t time_us = 0;
};

struct InputFile {
    AVFormatContext* context = nullptr;
    ~InputFile() { avformat_close_input(&context); }
};
struct Packet {
    AVPacket* value = av_packet_alloc();
    Packet() { CHECK(value); }
    ~Packet() { av_packet_free(&value); }
};
struct Filter {
    AVBSFContext* context = nullptr;
    ~Filter() { av_bsf_free(&context); }
};

inline std::vector<TimedFrame> LoadFixture(const char* path) {
    InputFile input;
    CHECK(avformat_open_input(&input.context, path, nullptr, nullptr) == 0);
    CHECK(avformat_find_stream_info(input.context, nullptr) >= 0);
    const int video = av_find_best_stream(input.context, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const int audio = av_find_best_stream(input.context, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    CHECK(video >= 0 && audio >= 0);
    Filter filter;
    CHECK(av_bsf_alloc(av_bsf_get_by_name("h264_mp4toannexb"), &filter.context) == 0);
    CHECK(avcodec_parameters_copy(filter.context->par_in, input.context->streams[video]->codecpar) == 0);
    filter.context->time_base_in = input.context->streams[video]->time_base;
    CHECK(av_bsf_init(filter.context) == 0);

    std::vector<TimedFrame> frames;
    auto append = [&](const AVPacket* packet, bool is_video) {
        const auto* stream = input.context->streams[is_video ? video : audio];
        const auto* parameters = stream->codecpar;
        CHECK(packet->dts != AV_NOPTS_VALUE);
        const auto time_us = av_rescale_q(packet->dts, stream->time_base, AVRational{1, 1000000});
        if (time_us < 0) return; // Skip the AAC encoder's priming packet.
        auto frame = std::make_shared<media::EncodedFrame>();
        frame->info.track_id = is_video ? 0 : 1;
        frame->info.media_type = is_video ? media::MediaType::Video : media::MediaType::Audio;
        frame->info.codec = is_video ? media::CodecType::H264 : media::CodecType::AAC;
        auto& timestamp = frame->info.timestamp;
        timestamp.time_base_num = 1;
        timestamp.time_base_den = is_video ? 90000 : parameters->sample_rate;
        timestamp.dts = timestamp.pts = av_rescale_q(packet->dts, stream->time_base,
            AVRational{1, timestamp.time_base_den});
        timestamp.receive_time_ms = timestamp.capture_time_ms = 1000 + time_us / 1000;
        timestamp.capture_time_valid = true;
        frame->rtp.ssrc = is_video ? 100 : 200;
        frame->buffer = common::SharedBuffer::Take(
            std::vector<uint8_t>(packet->data, packet->data + packet->size));
        if (is_video) {
            frame->frame_type = packet->flags & AV_PKT_FLAG_KEY
                ? media::EncodedFrameType::Key : media::EncodedFrameType::Delta;
            frame->video.width = parameters->width;
            frame->video.height = parameters->height;
        } else {
            frame->frame_type = media::EncodedFrameType::Audio;
            frame->sample_rate = parameters->sample_rate;
            frame->sample_count = 1024;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 24, 100)
            frame->channels = parameters->ch_layout.nb_channels;
#else
            frame->channels = parameters->channels;
#endif
            frame->codec_config = common::SharedBuffer::Take(std::vector<uint8_t>(
                parameters->extradata, parameters->extradata + parameters->extradata_size));
        }
        media::EncodedFrameEvent event;
        event.source.endpoint_id = 42; // Both tracks belong to the same publisher.
        event.source.session_id = "session";
        event.source.stream_id = "stream";
        event.source.track_id = frame->info.track_id;
        event.source.ssrc = frame->rtp.ssrc;
        event.frame = std::move(frame);
        frames.push_back({std::move(event), time_us});
    };

    Packet packet, filtered;
    int result;
    while ((result = av_read_frame(input.context, packet.value)) >= 0) {
        if (packet.value->stream_index == video) {
            CHECK(av_bsf_send_packet(filter.context, packet.value) == 0);
            int filtered_result;
            while ((filtered_result = av_bsf_receive_packet(filter.context, filtered.value)) >= 0) {
                append(filtered.value, true);
                av_packet_unref(filtered.value);
            }
            CHECK(filtered_result == AVERROR(EAGAIN));
        } else if (packet.value->stream_index == audio) {
            append(packet.value, false);
        }
        av_packet_unref(packet.value);
    }
    CHECK(result == AVERROR_EOF);
    CHECK(!frames.empty());
    return frames;
}

inline void Order(std::vector<TimedFrame>& frames, bool audio_first) {
    std::stable_sort(frames.begin(), frames.end(), [=](const TimedFrame& a, const TimedFrame& b) {
        if (a.time_us != b.time_us) return a.time_us < b.time_us;
        return audio_first ? a.event.source.track_id > b.event.source.track_id
                           : a.event.source.track_id < b.event.source.track_id;
    });
}

inline std::vector<media::EncodedFrameEvent> FirstTracks(const std::vector<TimedFrame>& frames) {
    std::vector<media::EncodedFrameEvent> result;
    for (auto type : {media::MediaType::Video, media::MediaType::Audio}) {
        const auto it = std::find_if(frames.begin(), frames.end(), [=](const TimedFrame& f) {
            return f.event.frame->info.media_type == type;
        });
        CHECK(it != frames.end());
        CHECK(service::Mp4Writer::Ready(*it->event.frame));
        result.push_back(it->event);
    }
    return result;
}

inline void VerifyMp4(const std::filesystem::path& path, const std::vector<TimedFrame>& frames) {
    InputFile output;
    CHECK(avformat_open_input(&output.context, path.string().c_str(), nullptr, nullptr) == 0);
    CHECK(avformat_find_stream_info(output.context, nullptr) >= 0);
    CHECK(output.context->nb_streams == 2);
    const int video = av_find_best_stream(output.context, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const int audio = av_find_best_stream(output.context, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    CHECK(video >= 0 && audio >= 0 && video != audio);
    CHECK(output.context->streams[video]->codecpar->codec_id == AV_CODEC_ID_H264);
    CHECK(output.context->streams[audio]->codecpar->codec_id == AV_CODEC_ID_AAC);
    size_t expected[2]{}, actual[2]{};
    for (const auto& frame : frames)
        ++expected[frame.event.frame->info.media_type == media::MediaType::Video ? 0 : 1];
    int64_t previous[2]{AV_NOPTS_VALUE, AV_NOPTS_VALUE};
    Packet packet;
    int result;
    while ((result = av_read_frame(output.context, packet.value)) >= 0) {
        const int index = packet.value->stream_index == video ? 0 : 1;
        CHECK(packet.value->stream_index == video || packet.value->stream_index == audio);
        CHECK(packet.value->dts != AV_NOPTS_VALUE);
        CHECK(previous[index] == AV_NOPTS_VALUE || packet.value->dts > previous[index]);
        previous[index] = packet.value->dts;
        ++actual[index];
        av_packet_unref(packet.value);
    }
    CHECK(result == AVERROR_EOF);
    CHECK(actual[0] == expected[0] && actual[1] == expected[1]);
}

} // namespace recording_test
