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

    if (packet.timestamp != current_.timestamp || packet.ssrc != current_.ssrc)
    {
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
    has_last_seq_ = false;
    last_seq_ = 0;

    return AppendPacket(packet);
}

bool RtpFrameAssembler::AppendPacket(const H264ParsedPacket& packet)
{
    if (has_last_seq_ && !IsSeqContinuous(last_seq_, packet.seq))
    {
        broken_ = true;
    }

    last_seq_ = packet.seq;
    has_last_seq_ = true;
    current_.last_seq = packet.seq;

    current_.keyframe = current_.keyframe || packet.has_key_nalu;
    current_.has_sps = current_.has_sps || packet.has_sps;
    current_.has_pps = current_.has_pps || packet.has_pps;
    current_.has_idr = current_.has_idr || packet.has_idr;

    for (const auto& unit : packet.units)
    {
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
    }

    current_ = H264AccessUnit{};
    has_current_ = false;
    broken_ = false;
    has_last_seq_ = false;
    last_seq_ = 0;

    return true;
}

} // namespace media
