#include "RtpFrameAssembler.h"
#include "logger.h"

namespace media
{

void RtpFrameAssembler::Reset()
{
    has_current_ = false;
    broken_ = false;
    has_last_seq_ = false;
    last_seq_ = 0;
    current_ = H264AccessUnit{};
    ready_frames_.clear();
}

bool RtpFrameAssembler::HasFrame() const
{
    return !ready_frames_.empty();
}

bool RtpFrameAssembler::PopFrame(H264AccessUnit& out)
{
    if (ready_frames_.empty())
    {
        return false;
    }

    out = std::move(ready_frames_.front());
    ready_frames_.pop_front();
    return true;
}

bool RtpFrameAssembler::Input(const H264ParsedPacket& packet)
{
    if (!packet.valid || packet.malformed)
    {
        return false;
    }

    if (!has_current_)
    {
        return StartNewFrame(packet);
    }

    // Timestamp changed before marker arrived.
    // Current frame is incomplete, drop it and start a new one.
    if (packet.timestamp != current_.timestamp || packet.ssrc != current_.ssrc)
    {
        LOG_INFO("[FrameAssembler] timestamp/ssrc switch before frame complete",
                 " old_ssrc=", current_.ssrc,
                 " old_ts=", current_.timestamp,
                 " new_ssrc=", packet.ssrc,
                 " new_ts=", packet.timestamp);

        has_current_ = false;
        broken_ = false;
        has_last_seq_ = false;
        current_ = H264AccessUnit{};

        return StartNewFrame(packet);
    }

    return AppendPacket(packet);
}

bool RtpFrameAssembler::StartNewFrame(const H264ParsedPacket& packet)
{
    current_ = H264AccessUnit{};
    current_.ssrc = packet.ssrc;
    current_.timestamp = packet.timestamp;
    current_.first_seq = packet.seq;
    current_.last_seq = packet.seq;
    current_.marker = packet.marker;

    current_.keyframe = packet.has_key_nalu;
    current_.has_sps = packet.has_sps;
    current_.has_pps = packet.has_pps;
    current_.has_idr = packet.has_idr;

    has_current_ = true;
    broken_ = false;
    has_last_seq_ = true;
    last_seq_ = packet.seq;

    return AppendPacket(packet);
}

bool RtpFrameAssembler::AppendPacket(const H264ParsedPacket& packet)
{
    if (has_last_seq_ && !IsSeqContinuous(last_seq_, packet.seq))
    {
        LOG_INFO("[FrameAssembler] seq gap",
                 " last=", last_seq_,
                 " cur=", packet.seq,
                 " ssrc=", packet.ssrc,
                 " ts=", packet.timestamp);

        broken_ = true;
    }

    last_seq_ = packet.seq;
    current_.last_seq = packet.seq;

    current_.keyframe = current_.keyframe || packet.has_key_nalu;
    current_.has_sps = current_.has_sps || packet.has_sps;
    current_.has_pps = current_.has_pps || packet.has_pps;
    current_.has_idr = current_.has_idr || packet.has_idr;

    for (const auto& unit : packet.units)
    {
        // Today only accept complete NAL units.
        // FU-A reassembly can be added in the next step.
        if (unit.unit_type == H264PayloadUnitType::SingleNalu ||
            unit.unit_type == H264PayloadUnitType::StapANalu)
        {
            current_.nalus.push_back(unit.data);
        }
    }

    if (packet.marker)
    {
        return FinishCurrentFrame();
    }

    return true;
}

bool RtpFrameAssembler::FinishCurrentFrame()
{
    current_.marker = true;
    current_.broken = broken_;
    current_.complete = !broken_ && !current_.nalus.empty();

    if (current_.complete)
    {
        ready_frames_.push_back(std::move(current_));
    }
    else
    {
        LOG_INFO("[FrameAssembler] drop broken frame",
                 " ssrc=", current_.ssrc,
                 " ts=", current_.timestamp,
                 " first_seq=", current_.first_seq,
                 " last_seq=", current_.last_seq,
                 " nalu_count=", current_.nalus.size());
    }

    current_ = H264AccessUnit{};
    has_current_ = false;
    broken_ = false;
    has_last_seq_ = false;
    last_seq_ = 0;

    return true;
}

} // namespace media