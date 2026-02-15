#include "RtcpReciver.h"

rtcpx::RtcpReceiverImpl::RtcpReceiverImpl(rtcpx::IRtcpObserver* observer)
    : observer_(observer) {}

rtcpx::RtcpReceiverImpl::~RtcpReceiverImpl() = default;

bool rtcpx::RtcpReceiverImpl::OnRtcpPacket(const uint8_t* data, size_t len)
{
    if (!data || len < 4) return false;

    size_t off = 0;
    while (off + 4 <= len)
    {
        uint8_t v_p_count = data[off];
        uint8_t pt = data[off + 1];
        uint16_t length = (data[off + 2] << 8) | data[off + 3]; /* length in 32-bit words, excluding header */

        size_t packet_size = (length + 1) * 4; /* total packet size in bytes */
        if (off + packet_size > len)
        {
            return false;
        }

        const uint8_t* packet_data = data + off;
        size_t packet_len = packet_size;

        switch (pt)
        {
            case 200: // SR
                break;

            case 201: // RR
                break;

            case 202: // SDES

                break;
        }

        off += packet_len;
    }

    return true;
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