#include "H264RtpDepacketizer.h"
#include "MutableBuffer.h"

#include <algorithm>
#include <utility>

namespace media
{
namespace
{
constexpr uint8_t kAnnexBStartCode[] = {0, 0, 0, 1};

void AppendAnnexBNalu(common::MutableBuffer& out, const common::ByteVector& nalu)
{
    if (nalu.empty()) return;
    out.Append(common::BufferView(kAnnexBStartCode, sizeof(kAnnexBStartCode)));
    out.Append(common::BufferView(nalu.data(), nalu.size()));
}
}

H264RtpDepacketizer::H264RtpDepacketizer(const std::string& fmtp)
    : depacketizer_(fmtp) {}

bool H264RtpDepacketizer::Input(const RtpView& packet)
{
    return depacketizer_.input(packet);
}

bool H264RtpDepacketizer::HasFrame() const
{
    return depacketizer_.hasFrame();
}

void H264RtpDepacketizer::Reset()
{
    depacketizer_.Reset();
}

bool H264RtpDepacketizer::PopFrame(EncodedFrame& out)
{
    H264AccessUnit au;
    if (!depacketizer_.popAccessUnit(au)) return false;

    EncodedFrame frame;
    frame.info.media_type = MediaType::Video;
    frame.info.codec = CodecType::H264;
    // Retain the existing DTS fallback for streams without frame reordering.
    // RTP alone does not provide DTS for streams containing B pictures.
    frame.info.timestamp.dts = au.timestamp;
    frame.info.timestamp.pts = au.timestamp;
    frame.info.timestamp.time_base_num = 1;
    frame.info.timestamp.time_base_den = 90000;
    frame.info.timestamp.receive_time_ms = au.receive_time_ms;
    frame.info.integrity = au.broken ? FrameIntegrity::Corrupted
        : (au.complete ? FrameIntegrity::Complete : FrameIntegrity::Incomplete);

    frame.rtp.ssrc = au.ssrc;
    frame.rtp.rtp_timestamp = au.timestamp;
    frame.rtp.first_sequence = au.first_seq;
    frame.rtp.last_sequence = au.last_seq;
    frame.rtp.packet_count = static_cast<uint16_t>(au.last_seq - au.first_seq) + 1;
    frame.sample_rate = 90000;

    frame.video.is_idr = au.has_idr;
    frame.video.has_sps = au.has_sps;
    frame.video.has_pps = au.has_pps;
    const auto* sps = depacketizer_.parameterSets().LatestSps();
    const auto* pps = depacketizer_.parameterSets().LatestPps();
    if (sps)
    {
        frame.video.width = sps->width;
        frame.video.height = sps->height;
    }

    const bool config_only = !au.has_idr && (au.has_sps || au.has_pps) &&
        std::all_of(au.nalus.begin(), au.nalus.end(), [](const common::ByteVector& nalu) {
            if (nalu.empty()) return false;
            const auto type = H264GetNalType(nalu[0]);
            return type == static_cast<uint8_t>(H264NalType::Sps) ||
                   type == static_cast<uint8_t>(H264NalType::Pps);
        });
    frame.frame_type = config_only ? EncodedFrameType::Config
        : (au.has_idr ? EncodedFrameType::Key : EncodedFrameType::Delta);

    const bool inject_sps = au.has_idr && !au.has_sps && sps;
    const bool inject_pps = au.has_idr && !au.has_pps && pps;
    size_t annexb_size = 0;
    for (const auto& nalu : au.nalus)
        annexb_size += sizeof(kAnnexBStartCode) + nalu.size();
    if (inject_sps) annexb_size += sizeof(kAnnexBStartCode) + sps->payload.size();
    if (inject_pps) annexb_size += sizeof(kAnnexBStartCode) + pps->payload.size();

    auto buffer = common::MutableBuffer::Allocate(annexb_size);
    buffer.Resize(0);
    if (inject_sps)
    {
        AppendAnnexBNalu(buffer, sps->payload);
        frame.video.has_sps = true;
    }
    if (inject_pps)
    {
        AppendAnnexBNalu(buffer, pps->payload);
        frame.video.has_pps = true;
    }
    frame.video.parameter_sets_injected = inject_sps || inject_pps;
    for (const auto& nalu : au.nalus) AppendAnnexBNalu(buffer, nalu);
    frame.buffer = std::move(buffer).Freeze();
    out = std::move(frame);
    return true;
}

} // namespace media
