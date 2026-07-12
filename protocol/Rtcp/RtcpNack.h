#ifndef _RTCP_NACK_H_
#define _RTCP_NACK_H_

#include <cstdint>
#include <vector>

namespace rtcpx 
{

class RtRtcpNack
{
public:
    struct NackPair
    {
        uint16_t pid = 0;
        uint16_t blp = 0;
    };
    /*
     * Build a complete RTCP RTPFB Generic NACK packet.
     *
     * Return empty vector if lost_seqs is empty.
     */
    static std::vector<uint8_t> Build(uint32_t sender_ssrc, uint32_t media_ssrc, const std::vector<uint16_t>& lost_seqs);

    /*
     * Convert lost RTP sequence numbers to PID/BLP pairs.
     */
    static std::vector<NackPair> PackPairs(const std::vector<uint16_t>& lost_seqs);

    /*
     * Expand PID/BLP pairs back to RTP sequence numbers.
     * Mostly used for unit test or local verification.
     */
    static std::vector<uint16_t> ExpandPairs(const std::vector<NackPair>& pairs);

    /*
     * Parse a complete RTCP RTPFB Generic NACK packet.
     *
     * This is optional for sender side, but useful for testing Build().
     */
    static bool Parse(const uint8_t* data, size_t len, uint32_t* sender_ssrc, uint32_t* media_ssrc, std::vector<uint16_t>* lost_seqs);

};



}


#endif /* _RTCP_NACK_H_ */