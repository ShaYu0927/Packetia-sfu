#include "Mp4Writer.h"
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/version.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
}
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>

namespace service {
namespace {
std::vector<uint8_t> ParameterSets(const media::EncodedFrame& frame) 
{
    std::vector<uint8_t> result;
    const auto* data = frame.Data();
    auto prefix = [&](size_t pos) -> size_t {
        if (pos + 3 <= frame.size && !data[pos] && !data[pos+1]) {
            if (data[pos+2] == 1) return 3;
            if (pos + 4 <= frame.size && !data[pos+2] && data[pos+3] == 1) return 4;
        }
        return 0;
    };
    bool sps = false, pps = false;
    for (size_t pos = 0; pos < frame.size;) 
    {
        const auto size = prefix(pos);
        if (!size) { ++pos; continue; }
        const auto start = pos + size;
        auto end = start;
        while (end < frame.size && !prefix(end)) ++end;
        if (start < end) {
            const auto type = data[start] & 31;
            if (type == 7 || type == 8) {
                result.insert(result.end(), {0, 0, 0, 1});
                result.insert(result.end(), data + start, data + end);
                sps |= type == 7;
                pps |= type == 8;
            }
        }
        pos = end;
    }
    return sps && pps ? result : std::vector<uint8_t>{};
}
}

bool Mp4Writer::Ready(const media::EncodedFrame& frame) 
{
    if (!frame.Valid() || !frame.IsComplete()) return false;
    if (frame.info.codec == media::CodecType::H264)
        return frame.IsKeyFrame() && frame.video.width > 0 && frame.video.height > 0 &&
               !ParameterSets(frame).empty();
    if (frame.info.codec == media::CodecType::AAC)
        return frame.sample_rate > 0 && frame.channels > 0 && frame.codec_config &&
               frame.codec_config->size() >= 2;
    return false;
}

Mp4Writer::~Mp4Writer() { Close(); }

bool Mp4Writer::Fail(const std::string& operation, int error) {
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(error, message, sizeof(message));
    error_ = operation + ": " + message;
    return false;
}

bool Mp4Writer::FailFile(const std::string& operation) {
    error_ = operation + ": " + (file_ ? file_->Error() : "file is not available");
    return false;
}

int Mp4Writer::WritePacket(void* opaque, uint8_t* data, int bytes) {
    if (!opaque || bytes < 0 || (!data && bytes)) return AVERROR(EINVAL);
    if (!bytes) return 0;
    auto* file = static_cast<ISeekableFile*>(opaque);
    const int result = file->Write(data, static_cast<uint64_t>(bytes));
    return result < 0 ? result : bytes;
}

int64_t Mp4Writer::Seek(void* opaque, int64_t offset, int whence) 
{
    if (!opaque) return AVERROR(EINVAL);
    auto* file = static_cast<ISeekableFile*>(opaque);
    if (whence & AVSEEK_SIZE) return file->Size();
    whence &= ~AVSEEK_FORCE;

    int64_t base = 0;
    if (whence == SEEK_CUR) base = file->Tell();
    else if (whence == SEEK_END) base = file->Size();
    else if (whence != SEEK_SET) return AVERROR(EINVAL);
    if (base < 0) return base;
    if ((offset > 0 && base > std::numeric_limits<int64_t>::max() - offset) ||
        offset == std::numeric_limits<int64_t>::min() ||
        (offset < 0 && base < -offset)) return AVERROR(EINVAL);
    const int64_t target = base + offset;
    if (target < 0) return AVERROR(EINVAL);
    const int result = file->Seek(target);
    return result < 0 ? result : file->Tell();
}

bool Mp4Writer::Open(std::unique_ptr<ISeekableFile> file,
                     const std::vector<media::EncodedFrameEvent>& tracks) {
    Close();
    error_.clear();
    if (!file || !file->IsOpen()) return Fail("open MP4", AVERROR(EINVAL));
    file_ = std::move(file);
    int result = avformat_alloc_output_context2(&context_, nullptr, "mp4", nullptr);
    if (result < 0 || !context_) return Fail("allocate MP4", result < 0 ? result : AVERROR(ENOMEM));
    context_->max_interleave_delta = 1000000;
    for (const auto& event : tracks) {
        const auto& frame = *event.frame;
        if (!Ready(frame)) return Fail("missing codec parameters", AVERROR(EINVAL));
        AVStream* stream = avformat_new_stream(context_, nullptr);
        if (!stream) return Fail("create track", AVERROR(ENOMEM));
        stream->time_base = {1, frame.info.codec == media::CodecType::H264 ? 90000 : static_cast<int>(frame.sample_rate)};
        auto* parameters = stream->codecpar;
        std::vector<uint8_t> config;
        if (frame.info.codec == media::CodecType::H264) 
        {
            parameters->codec_type = AVMEDIA_TYPE_VIDEO;
            parameters->codec_id = AV_CODEC_ID_H264;
            parameters->width = frame.video.width;
            parameters->height = frame.video.height;
            config = ParameterSets(frame);
        } 
        else 
        {
            parameters->codec_type = AVMEDIA_TYPE_AUDIO;
            parameters->codec_id = AV_CODEC_ID_AAC;
            parameters->sample_rate = frame.sample_rate;
            parameters->frame_size = frame.sample_count;
            // AVCodecParameters adopted AVChannelLayout in libavcodec 59.24.100.
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 24, 100)
            av_channel_layout_default(&parameters->ch_layout, frame.channels);
#else
            parameters->channels = frame.channels;
            parameters->channel_layout = av_get_default_channel_layout(frame.channels);
#endif
            config = *frame.codec_config;
        }
        parameters->extradata = static_cast<uint8_t*>(av_mallocz(config.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!parameters->extradata) return Fail("allocate codec parameters", AVERROR(ENOMEM));
        std::memcpy(parameters->extradata, config.data(), config.size());
        parameters->extradata_size = static_cast<int>(config.size());
        tracks_[event.source.endpoint_id].index = stream->index;
    }
    constexpr int io_buffer_size = 64 * 1024;
    auto* io_buffer = static_cast<unsigned char*>(av_malloc(io_buffer_size));
    if (!io_buffer) return Fail("allocate IO buffer", AVERROR(ENOMEM));
    io_context_ = avio_alloc_context(io_buffer, io_buffer_size, 1, file_.get(), nullptr, &Mp4Writer::WritePacket, &Mp4Writer::Seek);
    if (!io_context_) 
    {
        av_free(io_buffer);
        return Fail("create custom IO", AVERROR(ENOMEM));
    }
    io_context_->seekable = AVIO_SEEKABLE_NORMAL;
    context_->pb = io_context_;
    context_->flags |= AVFMT_FLAG_CUSTOM_IO;
    // Fragmented MP4 remains inspectable after completed fragments even if the
    // process exits abruptly. A normal Close still writes the final trailer.
    AVDictionary* options = nullptr;
    // Delay the initial moov until packet timestamps are known so each track's
    // start offset can be represented; empty_moov loses this A/V alignment.
    av_dict_set(&options, "movflags", "frag_keyframe+delay_moov+default_base_moof", 0);
    result = avformat_write_header(context_, &options);
    av_dict_free(&options);
    if (result < 0) return Fail("write MP4 header", result);
    header_written_ = true;
    return true;
}

bool Mp4Writer::Write(const media::EncodedFrameEvent& event, int64_t timestamp_us) 
{
    auto it = tracks_.find(event.source.endpoint_id);
    if (!header_written_ || it == tracks_.end() || timestamp_us < 0)
        return Fail("unknown track or invalid timestamp", AVERROR(EINVAL));
    auto& track = it->second;
    const auto* stream = context_->streams[track.index];
    const auto& frame = *event.frame;
    if (frame.size > static_cast<size_t>(std::numeric_limits<int>::max()))
        return Fail("oversized frame", AVERROR(EINVAL));
    auto* packet = av_packet_alloc();
    if (!packet) return Fail("allocate packet", AVERROR(ENOMEM));
    int result = av_new_packet(packet, static_cast<int>(frame.size));
    if (result < 0) { av_packet_free(&packet); return Fail("allocate payload", result); }
    std::memcpy(packet->data, frame.Data(), frame.size);
    packet->stream_index = track.index;
    packet->pts = packet->dts = av_rescale_q(timestamp_us, AVRational{1, 1000000}, stream->time_base);
    if (frame.IsKeyFrame()) packet->flags |= AV_PKT_FLAG_KEY;
    if (frame.sample_count && frame.info.media_type == media::MediaType::Audio)
        packet->duration = av_rescale_q(frame.sample_count, AVRational{1, static_cast<int>(frame.sample_rate)}, stream->time_base);
    if (track.pending) {
        if (packet->dts <= track.pending->dts) {
            av_packet_free(&packet);
            return Fail("non-increasing DTS (B frames are not supported by the RTP frame timestamps)", AVERROR(EINVAL));
        }
        track.last_duration = packet->dts - track.pending->dts;
        if (!track.pending->duration) track.pending->duration = track.last_duration;
        result = av_interleaved_write_frame(context_, track.pending);
        av_packet_free(&track.pending);
        if (result < 0) { av_packet_free(&packet); return Fail("write packet", result); }
    }
    track.pending = packet;
    return true;
}

bool Mp4Writer::Close() {
    bool ok = true;
    for (auto& entry : tracks_) {
        auto& track = entry.second;
        if (header_written_ && track.pending) {
            if (!track.pending->duration)
                track.pending->duration = track.last_duration > 0 ? track.last_duration :
                    av_rescale_q(40, AVRational{1, 1000}, context_->streams[track.index]->time_base);
            const int result = av_interleaved_write_frame(context_, track.pending);
            if (result < 0) ok = Fail("flush packet", result);
        }
        av_packet_free(&track.pending);
    }
    if (header_written_) {
        const int result = av_write_trailer(context_);
        if (result < 0) ok = Fail("write trailer", result);
    }
    if (io_context_) avio_flush(io_context_);
    if (file_ && file_->Flush() < 0) ok = FailFile("flush file");
    if (context_) {
        context_->pb = nullptr; // custom IO is owned below
        avformat_free_context(context_);
        context_ = nullptr;
    }
    if (io_context_) avio_context_free(&io_context_);
    if (file_ && file_->Close() < 0) ok = FailFile("close file");
    file_.reset();
    tracks_.clear();
    header_written_ = false;
    return ok;
}
}
