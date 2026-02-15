#ifndef _RTCPRECIVER_H_
#define _RTCPRECIVER_H_

#include "RtcpContext.h"


namespace rtcpx 
{
class RtcpReceiverImpl : public rtcpx::IRtcpReceiver
{
public:
    explicit RtcpReceiverImpl(rtcpx::IRtcpObserver* observer);
    ~RtcpReceiverImpl();

    bool OnRtcpPacket(const uint8_t* data, size_t len) override;

    void SetObserver(IRtcpObserver* obs) override;
    void SetLocalSsrc(uint32_t ssrc) override;
    void SetRemoteSsrc(uint32_t ssrc) override;

private:
    IRtcpObserver* observer_ = nullptr;
    uint32_t local_ssrc_ = 0;
    uint32_t remote_ssrc_ = 0;
};
}




#endif /* _RTCPRECVER_H_ */