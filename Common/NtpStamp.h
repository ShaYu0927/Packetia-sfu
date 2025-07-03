#ifndef _NTPSTAMP_H_
#define _NTPSTAMP_H_

#include <cstdint>
#include <memory>

class NtpStamp {
public:
    void setNtpStamp(uint32_t rtp_stamp, uint64_t ntp_stamp_ms);
    uint64_t getNtpStamp(uint32_t rtp_stamp, uint32_t sample_rate);
};


#endif // _NTPSTAMP_H_