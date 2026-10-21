#include "VideoCodecFactory.h"
#include "FfmpegVideoCodec.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/dict.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace media::codec {
namespace {
constexpr AVRational kTimeBase{1, 90000};
struct CodecDeleter { void operator()(AVCodecContext* p) const { avcodec_free_context(&p); } };
struct FrameDeleter { void operator()(AVFrame* p) const { av_frame_free(&p); } };
struct PacketDeleter { void operator()(AVPacket* p) const { av_packet_free(&p); } };
using CodecPtr = std::unique_ptr<AVCodecContext, CodecDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;

bool Fail(CodecError& error, CodecErrorCode code, const std::string& message)
{
    error = {code, message};
    return false;
}

bool Fail(CodecError& error, CodecErrorCode code, const char* operation, int result)
{
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(result, message, sizeof(message));
    return Fail(error, code, std::string(operation) + ": " + message);
}

size_t StartCode(const uint8_t* data, size_t size, size_t i)
{
    if (size - i >= 3 && data[i] == 0 && data[i + 1] == 0) {
        if (data[i + 2] == 1) return 3;
        if (size - i >= 4 && data[i + 2] == 0 && data[i + 3] == 1) return 4;
    }
    return 0;
}

void InspectNalus(EncodedFrame& frame)
{
    for (size_t i = 0; i < frame.Size();) {
        const auto prefix = StartCode(frame.Data(), frame.Size(), i);
        if (!prefix) { ++i; continue; }
        i += prefix;
        if (i == frame.Size()) break;
        if (frame.info.codec == CodecType::H264) {
            const auto type = frame.Data()[i] & 31;
            frame.video.has_sps |= type == 7;
            frame.video.has_pps |= type == 8;
            frame.video.is_idr |= type == 5;
        } else if (frame.info.codec == CodecType::H265 && frame.Size() - i >= 2) {
            const auto type = (frame.Data()[i] >> 1) & 63;
            frame.video.has_vps |= type == 32;
            frame.video.has_sps |= type == 33;
            frame.video.has_pps |= type == 34;
            frame.video.is_idr |= type == 19 || type == 20;
        }
    }
}

struct FfmpegStorage final : VideoFrameStorage
{
    FramePtr frame;
    explicit FfmpegStorage(AVFrame* value) : frame(value) {}
};

bool ExportFrame(const AVFrame& source, RawVideoFrame& output, CodecError& error)
{
    const auto format = static_cast<AVPixelFormat>(source.format);
    const auto* desc = av_pix_fmt_desc_get(format);
    if (!desc || (desc->flags & (AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_RGB)))
        return Fail(error, CodecErrorCode::Unsupported, "decoder output is not CPU YUV");
    RawVideoFrame result;
    if (format == AV_PIX_FMT_NV12) result.layout = PixelLayout::NV12;
    else if (desc->nb_components == 1) result.layout = PixelLayout::Gray;
    else if (desc->nb_components == 3 && (desc->flags & AV_PIX_FMT_FLAG_PLANAR)) {
        if (desc->log2_chroma_w == 1 && desc->log2_chroma_h == 1) result.layout = PixelLayout::I420;
        else if (desc->log2_chroma_w == 1 && desc->log2_chroma_h == 0) result.layout = PixelLayout::I422;
        else if (desc->log2_chroma_w == 0 && desc->log2_chroma_h == 0) result.layout = PixelLayout::I444;
        else return Fail(error, CodecErrorCode::Unsupported, "unsupported decoded YUV subsampling");
    } else return Fail(error, CodecErrorCode::Unsupported, "unsupported decoded pixel layout");
    auto storage = std::make_shared<FfmpegStorage>(av_frame_clone(&source));
    if (!storage->frame) return Fail(error, CodecErrorCode::RuntimeFailed, "retain raw video frame");
    result.storage = storage;
    for (int i = 0; i < 4; ++i) {
        result.planes[i] = storage->frame->data[i];
        result.strides[i] = storage->frame->linesize[i];
    }
    result.width = source.width;
    result.height = source.height;
    result.bit_depth = desc->comp[0].depth;
    result.big_endian = desc->flags & AV_PIX_FMT_FLAG_BE;
    result.full_range = source.color_range == AVCOL_RANGE_JPEG ||
        format == AV_PIX_FMT_YUVJ420P || format == AV_PIX_FMT_YUVJ422P || format == AV_PIX_FMT_YUVJ444P;
    switch (source.colorspace) {
    case AVCOL_SPC_BT709: result.matrix = ColorMatrix::Bt709; break;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M: result.matrix = ColorMatrix::Bt601; break;
    case AVCOL_SPC_SMPTE240M: result.matrix = ColorMatrix::Smpte240M; break;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL: result.matrix = ColorMatrix::Bt2020; break;
    default: break;
    }
    result.sar_num = source.sample_aspect_ratio.num;
    result.sar_den = source.sample_aspect_ratio.den;
    result.timestamp.pts = result.timestamp.dts = source.pts;
    result.timestamp.time_base_num = kTimeBase.num;
    result.timestamp.time_base_den = kTimeBase.den;
    output = std::move(result);
    return true;
}

AVPixelFormat PixelFormat(const RawVideoFrame& frame)
{
    std::string name;
    switch (frame.layout) {
    case PixelLayout::I420: name = "yuv420p"; break;
    case PixelLayout::I422: name = "yuv422p"; break;
    case PixelLayout::I444: name = "yuv444p"; break;
    case PixelLayout::Gray: name = "gray"; break;
    case PixelLayout::NV12: return frame.bit_depth == 8 ? AV_PIX_FMT_NV12 : AV_PIX_FMT_NONE;
    default: return AV_PIX_FMT_NONE;
    }
    if (frame.bit_depth != 8) name += std::to_string(frame.bit_depth) + (frame.big_endian ? "be" : "le");
    return av_get_pix_fmt(name.c_str());
}

bool ValidRawFrame(const RawVideoFrame& frame)
{
    if (!frame.storage || frame.width <= 0 || frame.height <= 0 ||
        !frame.timestamp.Valid() || frame.timestamp.pts == AV_NOPTS_VALUE) return false;
    const auto format = PixelFormat(frame);
    int minimum[4]{};
    if (format == AV_PIX_FMT_NONE || av_image_fill_linesizes(minimum, format, frame.width) < 0) return false;
    for (int i = 0; i < 4; ++i)
        if (minimum[i] && (!frame.planes[i] || std::abs(int64_t(frame.strides[i])) < minimum[i])) return false;
    return true;
}

class CodecState
{
protected:
    enum class State { Closed, Ready, Drained, Failed };
    State state_ = State::Closed;
    bool Ready(CodecError& error)
    {
        error = {};
        if (state_ == State::Ready) return true;
        return Fail(error, CodecErrorCode::RuntimeFailed, "codec is not ready; recreate it");
    }
    bool Failed(CodecError& error, const char* operation, int result)
    {
        state_ = State::Failed;
        return Fail(error, CodecErrorCode::RuntimeFailed, operation, result);
    }
    bool Failed(CodecError& error, const std::string& message)
    {
        state_ = State::Failed;
        return Fail(error, CodecErrorCode::RuntimeFailed, message);
    }
};

class FfmpegVideoDecoder final : public IVideoDecoder, private CodecState
{
public:
    bool Open(const VideoDecoderConfig& config, const std::string& name, CodecError& error)
    {
        config_ = config;
        name_ = name;
        const auto* codec = avcodec_find_decoder_by_name(name.c_str());
        if (!codec) return Fail(error, CodecErrorCode::Unsupported, "FFmpeg decoder is unavailable: " + name);
        decoder_.reset(avcodec_alloc_context3(codec));
        decoded_.reset(av_frame_alloc());
        input_.reset(av_packet_alloc());
        if (!decoder_ || !decoded_ || !input_)
            return Fail(error, CodecErrorCode::InitializationFailed, "allocate video decoder buffers");
        decoder_->pkt_timebase = kTimeBase;
        decoder_->thread_count = 1;
        decoder_->err_recognition = AV_EF_CAREFUL | AV_EF_EXPLODE;
        const int ret = avcodec_open2(decoder_.get(), codec, nullptr);
        if (ret < 0) return Fail(error, CodecErrorCode::InitializationFailed, "open video decoder", ret);
        state_ = State::Ready;
        return true;
    }

    bool Decode(const EncodedFrame& frame, std::vector<RawVideoFrame>& output, CodecError& error) override
    {
        if (!Ready(error)) return false;
        if (!frame.Valid() || !frame.IsComplete() || frame.info.media_type != MediaType::Video ||
            frame.info.codec != config_.codec || frame.info.timestamp.pts == AV_NOPTS_VALUE ||
            frame.Size() > size_t(std::numeric_limits<int>::max() - AV_INPUT_BUFFER_PADDING_SIZE))
            return Failed(error, "input must be a complete access unit of the configured codec with a valid PTS");
        const auto expected = BitstreamFor(config_.codec);
        // Existing H264 producers predate the representation field.
        const bool legacy_h264 = config_.codec == CodecType::H264 && frame.bitstream == EncodedBitstream::Unspecified;
        if (frame.bitstream != expected && !legacy_h264)
            return Failed(error, "input bitstream representation does not match the configured codec");
        if (expected == EncodedBitstream::AnnexB && !StartCode(frame.Data(), frame.Size(), 0))
            return Failed(error, "H264/H265 decoder requires Annex B access units");
        if (frame.IsConfigFrame()) {
            if (config_.codec == CodecType::VP8 || config_.codec == CodecType::VP9)
                return Failed(error, "VP8/VP9 have no separate parameter-set access unit");
            constexpr size_t kMaxPendingConfig = 1024 * 1024;
            if (frame.Size() > kMaxPendingConfig - pending_config_.size())
                return Failed(error, "pending video configuration exceeds 1 MiB");
            pending_config_.insert(pending_config_.end(), frame.Data(), frame.Data() + frame.Size());
            return true;
        }
        const auto size = pending_config_.size() + frame.Size();
        if (size > size_t(std::numeric_limits<int>::max() - AV_INPUT_BUFFER_PADDING_SIZE))
            return Failed(error, "video access unit with configuration is too large");
        int ret = av_new_packet(input_.get(), static_cast<int>(size));
        if (ret < 0) return Failed(error, "allocate padded input packet", ret);
        if (!pending_config_.empty()) std::memcpy(input_->data, pending_config_.data(), pending_config_.size());
        std::memcpy(input_->data + pending_config_.size(), frame.Data(), frame.Size());
        pending_config_.clear();
        const auto& ts = frame.info.timestamp;
        input_->pts = av_rescale_q(ts.pts, {ts.time_base_num, ts.time_base_den}, kTimeBase);
        if (input_->pts == AV_NOPTS_VALUE) {
            av_packet_unref(input_.get());
            return Failed(error, "decoder input PTS conversion overflow");
        }
        input_->dts = AV_NOPTS_VALUE;
        ret = avcodec_send_packet(decoder_.get(), input_.get());
        av_packet_unref(input_.get());
        if (ret < 0) return Failed(error, "send video access unit to decoder", ret);
        return Drain(output, false, error);
    }

    bool Flush(std::vector<RawVideoFrame>& output, CodecError& error) override
    {
        error = {};
        if (state_ == State::Drained) return true;
        if (!Ready(error)) return false;
        const int ret = avcodec_send_packet(decoder_.get(), nullptr);
        if (ret < 0) return Failed(error, "flush video decoder", ret);
        if (!Drain(output, true, error)) return false;
        state_ = State::Drained;
        return true;
    }

    void Close() noexcept override
    {
        decoder_.reset(); decoded_.reset(); input_.reset(); pending_config_.clear();
        state_ = State::Closed;
    }
    const char* Implementation() const noexcept override { return name_.c_str(); }

private:
    bool Drain(std::vector<RawVideoFrame>& output, bool flushing, CodecError& error)
    {
        for (;;) {
            const int ret = avcodec_receive_frame(decoder_.get(), decoded_.get());
            if (ret == AVERROR_EOF) return true;
            if (ret == AVERROR(EAGAIN)) return !flushing || Failed(error, "decoder did not finish draining");
            if (ret < 0) return Failed(error, "decode video frame", ret);
            if ((decoded_->flags & AV_FRAME_FLAG_CORRUPT) || decoded_->decode_error_flags)
                return Failed(error, "decoder reported a damaged video picture");
            if (decoded_->pts == AV_NOPTS_VALUE) return Failed(error, "decoded PTS is unavailable");
            RawVideoFrame frame;
            if (!ExportFrame(*decoded_, frame, error)) { state_ = State::Failed; return false; }
            output.push_back(std::move(frame));
            av_frame_unref(decoded_.get());
        }
    }
    VideoDecoderConfig config_;
    std::string name_;
    CodecPtr decoder_;
    FramePtr decoded_;
    PacketPtr input_;
    std::vector<uint8_t> pending_config_;
};

class FfmpegFrameScaler final : public IVideoFrameScaler
{
public:
    ~FfmpegFrameScaler() override { sws_freeContext(scaler_); }
    bool Open(int width, int height, CodecError& error)
    {
        canvas_.reset(av_frame_alloc());
        if (!canvas_) return Fail(error, CodecErrorCode::InitializationFailed, "allocate scaled frame");
        canvas_->format = AV_PIX_FMT_YUV420P;
        canvas_->width = width;
        canvas_->height = height;
        canvas_->sample_aspect_ratio = {1, 1};
        canvas_->color_range = AVCOL_RANGE_MPEG;
        const int ret = av_frame_get_buffer(canvas_.get(), 32);
        return ret >= 0 || Fail(error, CodecErrorCode::InitializationFailed, "allocate scaled video frame", ret);
    }

    bool Scale(const RawVideoFrame& src, RawVideoFrame& output, CodecError& error) override
    {
        error = {};
        if (!ValidRawFrame(src)) return Fail(error, CodecErrorCode::InvalidConfiguration, "invalid CPU video frame");
        int ret = av_frame_make_writable(canvas_.get());
        if (ret < 0) return Fail(error, CodecErrorCode::RuntimeFailed, "make scaled frame writable", ret);
        const double aspect = double(src.width) / src.height *
            (src.sar_num > 0 && src.sar_den > 0 ? double(src.sar_num) / src.sar_den : 1.0);
        int width = canvas_->width, height = canvas_->height;
        if (aspect > double(width) / height) height = std::clamp(int(width / aspect) & ~1, 2, height);
        else width = std::clamp(int(height * aspect) & ~1, 2, width);
        const int x = ((canvas_->width - width) / 2) & ~1;
        const int y = ((canvas_->height - height) / 2) & ~1;
        ptrdiff_t strides[4]{};
        for (int i = 0; i < 4; ++i) strides[i] = canvas_->linesize[i];
        ret = av_image_fill_black(canvas_->data, strides, AV_PIX_FMT_YUV420P,
                                  AVCOL_RANGE_MPEG, canvas_->width, canvas_->height);
        if (ret < 0) return Fail(error, CodecErrorCode::RuntimeFailed, "fill output canvas", ret);
        scaler_ = sws_getCachedContext(scaler_, src.width, src.height, PixelFormat(src),
            width, height, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!scaler_) return Fail(error, CodecErrorCode::RuntimeFailed, "allocate video scaler");
        int matrix = SWS_CS_DEFAULT;
        switch (src.matrix) {
        case ColorMatrix::Bt709: matrix = SWS_CS_ITU709; break;
        case ColorMatrix::Bt601: matrix = SWS_CS_ITU601; break;
        case ColorMatrix::Smpte240M: matrix = SWS_CS_SMPTE240M; break;
        case ColorMatrix::Bt2020: matrix = SWS_CS_BT2020; break;
        default: break;
        }
        const auto* coefficients = sws_getCoefficients(matrix);
        ret = sws_setColorspaceDetails(scaler_, coefficients, src.full_range, coefficients, 0, 0, 1 << 16, 1 << 16);
        if (ret < 0) return Fail(error, CodecErrorCode::RuntimeFailed, "configure scaler color range", ret);
        uint8_t* planes[4]{
            canvas_->data[0] + y * canvas_->linesize[0] + x,
            canvas_->data[1] + (y / 2) * canvas_->linesize[1] + x / 2,
            canvas_->data[2] + (y / 2) * canvas_->linesize[2] + x / 2, nullptr};
        ret = sws_scale(scaler_, src.planes.data(), src.strides.data(), 0, src.height, planes, canvas_->linesize);
        if (ret != height) return Fail(error, CodecErrorCode::RuntimeFailed, "scale decoded frame");
        canvas_->pts = av_rescale_q(src.timestamp.pts,
            {src.timestamp.time_base_num, src.timestamp.time_base_den}, kTimeBase);
        if (canvas_->pts == AV_NOPTS_VALUE)
            return Fail(error, CodecErrorCode::RuntimeFailed, "scaler PTS conversion overflow");
        return ExportFrame(*canvas_, output, error);
    }

private:
    FramePtr canvas_;
    SwsContext* scaler_ = nullptr;
};

bool ValidEncoding(const VideoEncoderConfig& config)
{
    return config.width >= 2 && config.width <= 4096 && !(config.width & 1) &&
        config.height >= 2 && config.height <= 4096 && !(config.height & 1) &&
        config.bitrate_bps > 0 && config.bitrate_bps <= 100000000 &&
        config.frame_rate > 0 && config.frame_rate <= 240 && config.keyframe_interval > 0;
}

class FfmpegVideoEncoder final : public IVideoEncoder, private CodecState
{
public:
    bool Open(const VideoEncoderConfig& config, const std::string& name, CodecError& error)
    {
        config_ = config;
        name_ = name;
        const auto* codec = avcodec_find_encoder_by_name(name.c_str());
        if (!codec) return Fail(error, CodecErrorCode::Unsupported, "FFmpeg encoder is unavailable: " + name);
        encoder_.reset(avcodec_alloc_context3(codec));
        packet_.reset(av_packet_alloc());
        if (!encoder_ || !packet_) return Fail(error, CodecErrorCode::InitializationFailed, "allocate video encoder buffers");
        auto& ctx = *encoder_;
        ctx.width = config.width;
        ctx.height = config.height;
        ctx.pix_fmt = AV_PIX_FMT_YUV420P;
        ctx.sample_aspect_ratio = {1, 1};
        ctx.time_base = kTimeBase;
        ctx.framerate = {config.frame_rate, 1};
        ctx.bit_rate = config.bitrate_bps;
        ctx.gop_size = config.keyframe_interval;
        ctx.max_b_frames = 0;
        ctx.thread_count = 1;
        ctx.color_range = AVCOL_RANGE_MPEG;
        AVDictionary* options = nullptr;
        if (name == "libx264") {
            const auto* parameters = std::get_if<H264Parameters>(&config.parameters);
            const auto profile = parameters ? parameters->profile : H264Profile::Baseline;
            const char* profile_name = profile == H264Profile::High ? "high" :
                (profile == H264Profile::Main ? "main" : "baseline");
            av_dict_set(&options, "preset", "ultrafast", 0);
            av_dict_set(&options, "tune", "zerolatency", 0);
            av_dict_set(&options, "profile", profile_name, 0);
            const char* params = profile == H264Profile::High
                ? "annexb=1:repeat-headers=1:scenecut=0:cabac=1:8x8dct=1"
                : (profile == H264Profile::Main
                    ? "annexb=1:repeat-headers=1:scenecut=0:cabac=1:8x8dct=0"
                    : "annexb=1:repeat-headers=1:scenecut=0");
            av_dict_set(&options, "x264-params", params, 0);
        } else if (name == "libx265") {
            av_dict_set(&options, "preset", "ultrafast", 0);
            av_dict_set(&options, "tune", "zerolatency", 0);
            av_dict_set(&options, "profile", "main", 0);
            av_dict_set(&options, "x265-params",
                "annexb=1:repeat-headers=1:scenecut=0:bframes=0:open-gop=0:pools=none:frame-threads=1:wpp=0:log-level=error", 0);
        } else if (name == "libvpx" || name == "libvpx-vp9") {
            av_dict_set(&options, "deadline", "realtime", 0);
            av_dict_set(&options, "cpu-used", "8", 0);
            av_dict_set(&options, "lag-in-frames", "0", 0);
            av_dict_set(&options, "auto-alt-ref", "0", 0);
            ctx.profile = 0;
        } else if (name == "libaom-av1") {
            av_dict_set(&options, "usage", "realtime", 0);
            av_dict_set(&options, "cpu-used", "8", 0);
            av_dict_set(&options, "lag-in-frames", "0", 0);
            ctx.profile = 0;
        } else if (name == "libsvtav1") {
            // SVT low-delay prediction requires CBR; bit_rate alone selects
            // VBR in the FFmpeg wrapper and fails initialization.
            ctx.rc_min_rate = ctx.rc_max_rate = ctx.bit_rate;
            ctx.rc_buffer_size = config.bitrate_bps;
            av_dict_set(&options, "preset", "8", 0);
            av_dict_set(&options, "svtav1-params", "lp=1:lookahead=0:enable-tf=0:pred-struct=1:rc=2", 0);
            ctx.profile = 0;
        }
        const int ret = avcodec_open2(encoder_.get(), codec, &options);
        const bool unused_options = av_dict_count(options) != 0;
        av_dict_free(&options);
        if (ret < 0) return Fail(error, CodecErrorCode::InitializationFailed, ("open " + name).c_str(), ret);
        if (unused_options) return Fail(error, CodecErrorCode::InitializationFailed, "encoder did not accept all adapter options: " + name);
        state_ = State::Ready;
        return true;
    }

    bool Encode(const RawVideoFrame& frame, std::vector<EncodedFrame::Ptr>& output, CodecError& error) override
    {
        if (!Ready(error)) return false;
        if (!ValidRawFrame(frame) || frame.width != config_.width || frame.height != config_.height ||
            frame.layout != PixelLayout::I420 || frame.bit_depth != 8 || frame.full_range ||
            (frame.sar_num > 0 && frame.sar_den > 0 && frame.sar_num != frame.sar_den))
            return Failed(error, "encoder adapter requires configured-size limited-range square-pixel I420");
        FramePtr input;
        const auto native = std::dynamic_pointer_cast<const FfmpegStorage>(frame.storage);
        bool same_views = native && native->frame->format == AV_PIX_FMT_YUV420P &&
            native->frame->width == frame.width && native->frame->height == frame.height;
        if (native) for (int i = 0; i < 3; ++i)
            same_views &= frame.planes[i] == native->frame->data[i] && frame.strides[i] == native->frame->linesize[i];
        if (same_views) input.reset(av_frame_clone(native->frame.get()));
        else {
            // A different backend may provide CPU planes. Copy them into an
            // owning FFmpeg frame; the common interface stays backend neutral.
            input.reset(av_frame_alloc());
            if (input) {
                input->width = frame.width; input->height = frame.height;
                input->format = AV_PIX_FMT_YUV420P;
                const int ret = av_frame_get_buffer(input.get(), 32);
                if (ret < 0) return Failed(error, "allocate encoder input", ret);
                av_image_copy(input->data, input->linesize, frame.planes.data(), frame.strides.data(),
                              AV_PIX_FMT_YUV420P, frame.width, frame.height);
            }
        }
        if (!input) return Failed(error, "retain encoder input");
        input->pts = av_rescale_q(frame.timestamp.pts,
            {frame.timestamp.time_base_num, frame.timestamp.time_base_den}, kTimeBase);
        if (input->pts == AV_NOPTS_VALUE) return Failed(error, "encoder input PTS conversion overflow");
        input->pict_type = AV_PICTURE_TYPE_NONE;
        input->sample_aspect_ratio = {1, 1};
        input->color_range = AVCOL_RANGE_MPEG;
        const int ret = avcodec_send_frame(encoder_.get(), input.get());
        if (ret < 0) return Failed(error, "send frame to video encoder", ret);
        return Drain(output, false, error);
    }

    bool Flush(std::vector<EncodedFrame::Ptr>& output, CodecError& error) override
    {
        error = {};
        if (state_ == State::Drained) return true;
        if (!Ready(error)) return false;
        const int ret = avcodec_send_frame(encoder_.get(), nullptr);
        if (ret < 0) return Failed(error, "flush video encoder", ret);
        if (!Drain(output, true, error)) return false;
        state_ = State::Drained;
        return true;
    }

    void Close() noexcept override { encoder_.reset(); packet_.reset(); state_ = State::Closed; }
    const char* Implementation() const noexcept override { return name_.c_str(); }

private:
    bool Drain(std::vector<EncodedFrame::Ptr>& output, bool flushing, CodecError& error)
    {
        for (;;) {
            const int ret = avcodec_receive_packet(encoder_.get(), packet_.get());
            if (ret == AVERROR_EOF) return true;
            if (ret == AVERROR(EAGAIN)) return !flushing || Failed(error, "encoder did not finish draining");
            if (ret < 0) return Failed(error, "receive encoded video packet", ret);
            auto frame = std::make_shared<EncodedFrame>();
            frame->info.media_type = MediaType::Video;
            frame->info.codec = config_.codec;
            frame->bitstream = BitstreamFor(config_.codec);
            frame->info.timestamp.pts = packet_->pts;
            frame->info.timestamp.dts = packet_->dts;
            frame->info.timestamp.time_base_num = kTimeBase.num;
            frame->info.timestamp.time_base_den = kTimeBase.den;
            frame->sample_rate = kTimeBase.den;
            frame->video.width = config_.width;
            frame->video.height = config_.height;
            frame->frame_type = packet_->flags & AV_PKT_FLAG_KEY ? EncodedFrameType::Key : EncodedFrameType::Delta;
            frame->buffer = common::SharedBuffer::Copy(packet_->data, packet_->size);
            if (frame->bitstream == EncodedBitstream::AnnexB) InspectNalus(*frame);
            output.push_back(std::move(frame));
            av_packet_unref(packet_.get());
        }
    }
    VideoEncoderConfig config_;
    std::string name_;
    CodecPtr encoder_;
    PacketPtr packet_;
};

} // namespace

std::unique_ptr<IVideoDecoder> CreateFfmpegDecoder(
    const VideoDecoderConfig& config, const std::string& name, CodecError& error)
{
    auto decoder = std::make_unique<FfmpegVideoDecoder>();
    if (!decoder->Open(config, name, error)) return nullptr;
    return decoder;
}

std::unique_ptr<IVideoEncoder> CreateFfmpegEncoder(
    const VideoEncoderConfig& config, const std::string& name, CodecError& error)
{
    auto encoder = std::make_unique<FfmpegVideoEncoder>();
    if (!encoder->Open(config, name, error)) return nullptr;
    return encoder;
}

std::unique_ptr<IVideoFrameScaler> VideoCodecFactory::CreateScaler(int width, int height, CodecError& error)
{
    error = {};
    VideoEncoderConfig config;
    config.width = width; config.height = height; config.bitrate_bps = 1;
    if (!ValidEncoding(config)) {
        Fail(error, CodecErrorCode::InvalidConfiguration, "invalid video scaler dimensions");
        return nullptr;
    }
    auto scaler = std::make_unique<FfmpegFrameScaler>();
    if (!scaler->Open(width, height, error)) return nullptr;
    return scaler;
}

} // namespace media::codec
