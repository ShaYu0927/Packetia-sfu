#include "H264Depacketizer.h"


bool H264Depacketizer::input(const RtpView& pkt)
{
    if (!pkt.valid())
    {
        return false;
    }

    media::H264ParsedPacket parsed;

    if (!parser_.Parse(pkt, parsed))
    {
        return false;
    }

    if (!parsed.valid)
    {
        return false;
    }

    auto result = packet_buffer_.InsertPacket(std::move(parsed));
    for (auto& frame : result.frames)
    {
        parameter_sets_.UpdateAccessUnit(frame);
        ready_frames_.emplace_back(std::move(frame));
    }

    return result.inserted || result.duplicate;
}


bool H264Depacketizer::hasFrame() const
{
    return !ready_frames_.empty();
}

std::vector<uint8_t> H264Depacketizer::popFrame()
{
    if (ready_frames_.empty())
    {
        return {};
    }
    media::H264AccessUnit au = std::move(ready_frames_.front());
    ready_frames_.pop_front();
    return au.ToAnnexB();
}

bool H264Depacketizer::popAccessUnit(media::H264AccessUnit& out)
{
    if (ready_frames_.empty())
        return false;
    out = std::move(ready_frames_.front());
    ready_frames_.pop_front();
    return true;
}
