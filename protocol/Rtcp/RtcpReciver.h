#ifndef _RTCPRECIVER_H_
#define _RTCPRECIVER_H_

#include "RtcpContext.h"


namespace rtcpx 
{
// Concrete RTCP receiver/parser.
// It consumes one compound RTCP buffer, validates each sub-packet, then emits
// normalized callbacks through IRtcpObserver. It does not own the observer.
class RtcpReceiverImpl : public rtcpx::IRtcpReceiver
{
public:
    explicit RtcpReceiverImpl(rtcpx::IRtcpObserver* observer);
    ~RtcpReceiverImpl();

    // Parse one compound RTCP packet. A compound packet may include SR/RR,
    // SDES, BYE, RTPFB, PSFB, etc. Return false on malformed framing.
    bool OnRtcpPacket(const uint8_t* data, size_t len) override;

    // Parse a single RTCP sub-packet whose common header has already been
    // validated by OnRtcpPacket().
    void HandleSingleRtcpPacket(const uint8_t* p,size_t len, uint8_t fmt, uint8_t pt);

    void SetObserver(IRtcpObserver* obs) override;
    void SetLocalSsrc(uint32_t ssrc) override;
    void SetRemoteSsrc(uint32_t ssrc) override;

private:
    IRtcpObserver* observer_ = nullptr;

    // Optional SSRC hints for future routing/RTT logic. The current parser only
    // stores them; packet handlers still trust SSRC values carried on the wire.
    uint32_t local_ssrc_ = 0;
    uint32_t remote_ssrc_ = 0;
};
}




#endif /* _RTCPRECVER_H_ */
