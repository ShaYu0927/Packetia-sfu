#include "H264Depacketizer.h"
#include "Rtp.h"    



bool H264Depacketizer::input(const RtpView& pkt)
{
    if (!pkt.valid()) return false;

    const uint8_t* payload = pkt.payload;
    size_t payload_len = pkt.payload_len;

    if(!has_ts)
    {
        has_ts = true;
        if(pkt.ts != cur_ts_)
        {
            reset_stream(pkt.ts, pkt.ts);
        }
    }
    else if (pkt.ts != cur_ts_)
    {
        if (!au_.empty()) 
        {
            flush_frame();
            au_.clear();
        }
        cur_ts_ = pkt.ts;
        assembling_fu_ = false;
    }
    else
    {
        reset_stream(pkt.ts, pkt.ts);
    }

    fu_nal_type_ = payload[0] & 0x1F;

    return true;
}

bool H264Depacketizer::hasFrame() const
{
    return false;
}

std::vector<uint8_t> H264Depacketizer::popFrame()
{
    return {};
}