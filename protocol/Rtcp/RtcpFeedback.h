#ifndef _RTCP_FEEDBACK_H_
#define _RTCP_FEEDBACK_H_

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rtcpx
{
class RtcpFeedback
{
public:
    static std::vector<uint8_t> BuildPli(uint32_t sender_ssrc, uint32_t media_ssrc);
    static bool ParsePli(const uint8_t* data, size_t len, uint32_t* sender_ssrc, uint32_t* media_ssrc);
};
}

#endif /* _RTCP_FEEDBACK_H_ */
