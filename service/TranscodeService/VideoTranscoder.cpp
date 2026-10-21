#include "VideoTranscoder.h"
#include "VideoCodecFactory.h"

extern "C" {
#include <libavutil/mathematics.h>
}

#include <algorithm>
#include <limits>
#include <utility>

namespace service::transcode {
namespace {
constexpr AVRational kTimeBase{1, 90000};
constexpr int64_t kNoTimestamp = std::numeric_limits<int64_t>::min();
namespace codec = media::codec;
} // namespace

struct VideoTranscoder::Impl
{
    enum class State { Closed, Ready, Drained, Failed };
    struct Output
    {
        VideoRendition config;
        std::unique_ptr<codec::IVideoEncoder> encoder;
        std::unique_ptr<codec::IVideoFrameScaler> scaler;
    };

    State state = State::Closed;
    VideoTranscodeConfig config;
    std::unique_ptr<codec::IVideoDecoder> decoder;
    std::vector<Output> outputs;
    std::string error;
    VideoTranscodeStats stats;
    bool timestamp_started = false;
    int64_t highest_rtp_timestamp = 0;
    int64_t last_decoded_pts = kNoTimestamp;

    bool Fail(const std::string& message)
    {
        error = message;
        state = State::Failed;
        // Also roll back partially opened pipelines. Retained output frames
        // keep their storage independently of these instances.
        outputs.clear();
        decoder.reset();
        return false;
    }

    void Append(Output& out, std::vector<media::EncodedFrame::Ptr>& encoded,
                std::vector<TranscodedFrame>& result)
    {
        for (auto& frame : encoded) {
            // Task/track identity belongs to orchestration, not the encoder.
            frame->info.track_id = out.config.track_id;
            result.push_back({out.config.id, std::move(frame)});
            ++stats.encoded_frames;
        }
    }

    bool Encode(const std::vector<codec::RawVideoFrame>& decoded, std::vector<TranscodedFrame>& result)
    {
        for (const auto& frame : decoded) {
            const auto pts = frame.timestamp.pts;
            if (pts == kNoTimestamp || (last_decoded_pts != kNoTimestamp && pts <= last_decoded_pts))
                return Fail("decoded PTS must be available and strictly increasing");
            last_decoded_pts = pts;
            ++stats.decoded_frames;
            for (auto& out : outputs) {
                codec::CodecError failure;
                codec::RawVideoFrame scaled;
                if (!out.scaler->Scale(frame, scaled, failure)) return Fail(failure.message);
                std::vector<media::EncodedFrame::Ptr> encoded;
                if (!out.encoder->Encode(scaled, encoded, failure)) return Fail(failure.message);
                Append(out, encoded, result);
            }
        }
        return true;
    }
};

VideoTranscoder::VideoTranscoder() : impl_(std::make_unique<Impl>()) {}
VideoTranscoder::~VideoTranscoder() = default;

bool VideoTranscoder::Open(const VideoTranscodeConfig& config)
{
    Close();
    auto& s = *impl_;
    s.error.clear();
    std::string error;
    if (!config.Validate(error)) return s.Fail(error);
    s.config = config;
    codec::CodecError failure;
    s.decoder = codec::VideoCodecFactory::CreateDecoder({config.input_codec}, config.decoder, failure);
    if (!s.decoder) return s.Fail(failure.message);
    for (const auto& rendition : config.renditions) {
        Impl::Output out;
        out.config = rendition;
        codec::VideoEncoderConfig encoding;
        encoding.codec = rendition.codec;
        encoding.parameters = rendition.parameters;
        encoding.width = rendition.encoding.width;
        encoding.height = rendition.encoding.height;
        encoding.bitrate_bps = rendition.encoding.bitrate_bps;
        encoding.frame_rate = config.frame_rate;
        encoding.keyframe_interval = config.keyframe_interval;
        out.encoder = codec::VideoCodecFactory::CreateEncoder(encoding, rendition.backend, failure);
        if (!out.encoder) return s.Fail(failure.message);
        out.scaler = codec::VideoCodecFactory::CreateScaler(encoding.width, encoding.height, failure);
        if (!out.scaler) return s.Fail(failure.message);
        s.outputs.push_back(std::move(out));
    }
    s.state = Impl::State::Ready;
    return true;
}

bool VideoTranscoder::Input(const media::EncodedFrame& frame, std::vector<TranscodedFrame>& output)
{
    auto& s = *impl_;
    if (s.state != Impl::State::Ready) return s.Fail("transcoder is not ready; call Open first");
    if (!frame.Valid() || !frame.IsComplete() || frame.info.timestamp.pts == kNoTimestamp)
        return s.Fail("input must be a complete encoded frame with a valid PTS");
    auto normalized = frame; // Shared encoded storage; no bitstream copy here.
    if (!frame.IsConfigFrame()) {
        const auto& ts = frame.info.timestamp;
        int64_t pts = ts.pts;
        if (s.config.timestamp_mode == InputTimestampMode::Rtp32) {
            if (ts.time_base_num != 1 || ts.time_base_den != 90000 || pts < 0 || pts > UINT32_MAX)
                return s.Fail("Rtp32 mode requires a raw uint32 PTS in 1/90000 units");
            if (s.timestamp_started) {
                int64_t delta = uint32_t(pts) - uint32_t(s.highest_rtp_timestamp);
                // Subtract in modulo-2^32 space, then choose the nearest epoch.
                delta = uint32_t(delta);
                if (delta >= INT64_C(0x80000000)) delta -= INT64_C(0x100000000);
                if (delta > 0 && s.highest_rtp_timestamp > INT64_MAX - delta)
                    return s.Fail("extended RTP timestamp overflow");
                pts = s.highest_rtp_timestamp + delta;
            }
            s.highest_rtp_timestamp = s.timestamp_started ? std::max(pts, s.highest_rtp_timestamp) : pts;
            s.timestamp_started = true;
        } else {
            pts = av_rescale_q(pts, {ts.time_base_num, ts.time_base_den}, kTimeBase);
            if (pts == kNoTimestamp) return s.Fail("input PTS conversion overflow");
        }
        normalized.info.timestamp.pts = normalized.info.timestamp.dts = pts;
        normalized.info.timestamp.time_base_num = kTimeBase.num;
        normalized.info.timestamp.time_base_den = kTimeBase.den;
    }
    codec::CodecError failure;
    std::vector<codec::RawVideoFrame> decoded;
    if (!s.decoder->Decode(normalized, decoded, failure)) return s.Fail(failure.message);
    ++s.stats.input_access_units;
    return s.Encode(decoded, output);
}

bool VideoTranscoder::Flush(std::vector<TranscodedFrame>& output)
{
    auto& s = *impl_;
    if (s.state == Impl::State::Drained) return true;
    if (s.state != Impl::State::Ready) return s.Fail("transcoder is not ready; call Open first");
    codec::CodecError failure;
    std::vector<codec::RawVideoFrame> decoded;
    if (!s.decoder->Flush(decoded, failure)) return s.Fail(failure.message);
    if (!s.Encode(decoded, output)) return false;
    for (auto& out : s.outputs) {
        std::vector<media::EncodedFrame::Ptr> encoded;
        if (!out.encoder->Flush(encoded, failure)) return s.Fail(failure.message);
        s.Append(out, encoded, output);
    }
    s.state = Impl::State::Drained;
    return true;
}

void VideoTranscoder::Close() noexcept
{
    auto& s = *impl_;
    s.outputs.clear();
    s.decoder.reset();
    s.state = Impl::State::Closed;
    s.stats = {};
    s.timestamp_started = false;
    s.highest_rtp_timestamp = 0;
    s.last_decoded_pts = kNoTimestamp;
}

const std::string& VideoTranscoder::LastError() const noexcept { return impl_->error; }
VideoTranscodeStats VideoTranscoder::Stats() const noexcept { return impl_->stats; }

} // namespace service::transcode
