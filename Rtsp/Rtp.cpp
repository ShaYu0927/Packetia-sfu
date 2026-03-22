#include "Rtp.h"
#include <string>


static inline std::string hex8(uint8_t v)
{
    const char* H = "0123456789ABCDEF";
    std::string s = "00";
    s[0] = H[v >> 4];
    s[1] = H[v & 0x0F];
    return s;
}

void RtpTrack::setInterleavedChannel(uint8_t rtp_channel, uint8_t rtcp_channel)
{
    info_._rtsp_transport.interleaved_rtcp = rtcp_channel;
    info_._rtsp_transport.interleaved_rtp  = rtp_channel;
}


