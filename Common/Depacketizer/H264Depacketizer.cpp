#include "H264Depacketizer.h"
#include "logger.h"


bool H264Depacketizer::input(const RtpView& pkt)
{
    if (!pkt.valid())
    {
        LOG_ERROR("[H264Depacketizer] invalid rtp view",
                  " ssrc=", pkt.ssrc,
                  " seq=", pkt.seq,
                  " ts=", pkt.ts,
                  " marker=", pkt.marker,
                  " payload_len=", pkt.payload_len);
        return false;
    }

    media::H264ParsedPacket parsed;

    if (!parser_.Parse(pkt, parsed))
    {
        LOG_ERROR("[H264Depacketizer] parse h264 rtp payload failed",
                  " ssrc=", pkt.ssrc,
                  " seq=", pkt.seq,
                  " ts=", pkt.ts,
                  " marker=", pkt.marker,
                  " payload_len=", pkt.payload_len);
        return false;
    }

    if (!parsed.valid)
    {
        LOG_ERROR("[H264Depacketizer] parsed packet is invalid",
                  " ssrc=", pkt.ssrc,
                  " seq=", pkt.seq,
                  " ts=", pkt.ts);
        return false;
    }

    if (!assembler_.Input(parsed))
    {
        LOG_ERROR("[H264Depacketizer] assemble frame failed",
                  " ssrc=", parsed.ssrc,
                  " seq=", parsed.seq,
                  " ts=", parsed.timestamp,
                  " marker=", parsed.marker);
        return false;
    }

    return true;
}

bool H264Depacketizer::hasFrame() const
{
    return true;
}

std::vector<uint8_t> H264Depacketizer::popFrame()
{
    std::vector<uint8_t> r;
    return r;
}







