#include "RtcpReciver.h"
#include "BufferRead.h"

rtcpx::RtcpReceiverImpl::RtcpReceiverImpl(rtcpx::IRtcpObserver* observer)
    : observer_(observer) {}

rtcpx::RtcpReceiverImpl::~RtcpReceiverImpl() = default;

bool rtcpx::RtcpReceiverImpl::OnRtcpPacket(const uint8_t* data, size_t len)
{
    if (!data || len < 4)
        return false;

    size_t offset = 0;
    bool parsed_any = false;

    while (offset + 4 <= len)
    {
        const uint8_t* p = data + offset;

        const uint8_t version = (p[0] >> 6) & 0x03;
        if (version != 2)
            return false;

        const uint8_t count_or_fmt = p[0] & 0x1F;
        const uint8_t pt = p[1];
        const uint16_t words_minus1 = ReadUint16BE(p + 2);
        const size_t pkt_len = (static_cast<size_t>(words_minus1) + 1) * 4;

        if (pkt_len < 4 || offset + pkt_len > len)
            return false;

        HandleSingleRtcpPacket(p, pkt_len, count_or_fmt, pt);

        offset += pkt_len;
        parsed_any = true;
    }

    return parsed_any;
}

void rtcpx::RtcpReceiverImpl::SetObserver(IRtcpObserver* obs)
{
    observer_ = obs;
}
void rtcpx::RtcpReceiverImpl::SetLocalSsrc(uint32_t ssrc)
{
    local_ssrc_ = ssrc;
}
void rtcpx::RtcpReceiverImpl::SetRemoteSsrc(uint32_t ssrc)
{
    remote_ssrc_ = ssrc;
}

void rtcpx::RtcpReceiverImpl::HandleSingleRtcpPacket(const uint8_t* p,
                                              size_t len,
                                              uint8_t fmt,
                                              uint8_t pt)
{
    switch (pt)
    {
    case 200: // SR
        
        break;
    case 201: // RR
        
        break;
    case 202: // SDES
        
        break;
    case 203: // BYE
       
        break;
    case 205: // RTPFB
        
        break;
    case 206: // PSFB
        
        break;
    default:
        break;
    }
}