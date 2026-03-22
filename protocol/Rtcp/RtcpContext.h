#ifndef _RTCPCONTEXT_H_
#define _RTCPCONTEXT_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace rtcpx 
{
    struct RrBlock 
    {
        uint32_t ssrc = 0;
        uint8_t  fraction_lost = 0;
        int32_t  cumulative_lost = 0;   // 24-bit signed in RTCP
        uint32_t extended_highest_seq = 0;
        uint32_t jitter = 0;
        uint32_t lsr = 0;
        uint32_t dlsr = 0;
    };

    struct ReceiverStats 
    {
        uint8_t  fraction_lost = 0;
        int32_t  cumulative_lost = 0;
        uint32_t extended_highest_seq = 0;
        uint32_t jitter = 0;
        int32_t  rtt_ms = -1;           // optional
    };

class IRtcpObserver
{
public:
    virtual ~IRtcpObserver() = default;

    virtual void OnReceiverReport(uint32_t sender_ssrc,
                                  uint32_t media_ssrc,
                                  uint8_t fraction_lost,
                                  int32_t cumulative_lost,
                                  uint32_t highest_seq,
                                  uint32_t jitter,
                                  uint32_t lsr,
                                  uint32_t dlsr) = 0;

    virtual void OnSenderReport(uint32_t sender_ssrc,
                                uint64_t ntp,
                                uint32_t rtp_ts,
                                uint32_t packet_count,
                                uint32_t octet_count) = 0;

    virtual void OnNack(uint32_t sender_ssrc,
                        uint32_t media_ssrc,
                        const uint16_t* seqs,
                        size_t count) = 0;

    virtual void OnPli(uint32_t sender_ssrc,
                       uint32_t media_ssrc) = 0;

    virtual void OnFir(uint32_t sender_ssrc,
                       uint32_t media_ssrc,
                       uint8_t seq_nr) = 0;

    virtual void OnBye(uint32_t sender_ssrc) = 0;

    virtual void OnRttUpdated(uint32_t media_ssrc,
                              uint32_t rtt_ms) = 0;
};

    /* ---------- Receiver (parse incoming RTCP) ---------- */
    class IRtcpReceiver 
    {
    public:
        virtual ~IRtcpReceiver() = default;

        /* Parse a (compound) RTCP packet. Return false if parse failed. */
        virtual bool OnRtcpPacket(const uint8_t* data, size_t len) = 0;

        /* Bind observer (not owned by receiver) */
        virtual void SetObserver(IRtcpObserver* obs) = 0;

        /* SSRC context (optional, but useful to interpret feedback) */
        virtual void SetLocalSsrc(uint32_t ssrc) = 0;   /* our sender SSRC (if needed) */
        virtual void SetRemoteSsrc(uint32_t ssrc) = 0;  /* peer SSRC (if tracking)     */
    };

    /* ---------- Sender (generate outgoing RTCP) ---------- */
    class IRtcpSender
    {
    public:
        virtual ~IRtcpSender() = default;
        /* Generate RTCP packet(s) to send. Return false if failed. */
        virtual bool GenerateRtcp(uint32_t media_ssrc, std::vector<uint8_t>& out) = 0;

        /* Build PLI packet (PSFB FMT=1). Returns bytes to send. */
        virtual std::vector<uint8_t> BuildPli(uint32_t sender_ssrc,
                                        uint32_t media_ssrc) = 0;

        std::unique_ptr<IRtcpReceiver> CreateRtcpReceiver();
        std::unique_ptr<IRtcpSender>   CreateRtcpSender(); 
    };

}


#endif /* _RTCPCONTEXT_H_ */