#ifndef _RTCPCONTEXT_H_
#define _RTCPCONTEXT_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace rtcpx 
{
    // One 24-byte reception report block carried by SR/RR packets.
    // It describes receiver-side quality for one media SSRC.
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

    // Normalized receiver quality values used by upper modules.
    // rtt_ms is optional because it can only be computed after SR/RR timing
    // information is available.
    struct ReceiverStats 
    {
        uint8_t  fraction_lost = 0;
        int32_t  cumulative_lost = 0;
        uint32_t extended_highest_seq = 0;
        uint32_t jitter = 0;
        int32_t  rtt_ms = -1;
    };

    // Lightweight inspection result used before full dispatch.
    // RTCP compound packets may contain several sub-packets; these fields keep
    // the first packet type for logging and the best media SSRC found for route.
    struct RtcpPacketInfo
    {
        bool valid = false;
        bool has_media_ssrc = false;
        bool has_sender_ssrc = false;

        uint8_t first_packet_type = 0;
        uint8_t first_count_or_fmt = 0;
        uint32_t sender_ssrc = 0;
        uint32_t media_ssrc = 0;
    };

    // Validate a compound RTCP packet and extract routing metadata.
    // This does not notify observers or parse all feedback details.
    bool InspectRtcpPacket(const uint8_t* data, size_t len, RtcpPacketInfo* out);

class IRtcpObserver
{
public:
    virtual ~IRtcpObserver() = default;

    // SR/RR report block: packet loss, highest sequence, jitter, and SR timing.
    virtual void OnReceiverReport(uint32_t sender_ssrc,
                                  uint32_t media_ssrc,
                                  uint8_t fraction_lost,
                                  int32_t cumulative_lost,
                                  uint32_t highest_seq,
                                  uint32_t jitter,
                                  uint32_t lsr,
                                  uint32_t dlsr) = 0;

    // Sender Report: sender wall-clock/RTP timestamp mapping and send counters.
    virtual void OnSenderReport(uint32_t sender_ssrc,
                                uint64_t ntp,
                                uint32_t rtp_ts,
                                uint32_t packet_count,
                                uint32_t octet_count) = 0;

    // Generic NACK feedback. seqs contains the expanded PID + BLP sequence list.
    virtual void OnNack(uint32_t sender_ssrc,
                        uint32_t media_ssrc,
                        const uint16_t* seqs,
                        size_t count) = 0;

    // Picture Loss Indication. Upper video modules should request a key frame.
    virtual void OnPli(uint32_t sender_ssrc,
                       uint32_t media_ssrc) = 0;

    // Full Intra Request with command sequence number.
    virtual void OnFir(uint32_t sender_ssrc,
                       uint32_t media_ssrc,
                       uint8_t seq_nr) = 0;

    // BYE indicates a participant/source is leaving.
    virtual void OnBye(uint32_t sender_ssrc) = 0;

    // Computed RTT update, if the receiver implementation can derive it.
    virtual void OnRttUpdated(uint32_t media_ssrc,
                              uint32_t rtt_ms) = 0;
};

    /* ---------- Receiver (parse incoming RTCP) ---------- */
    class IRtcpReceiver 
    {
    public:
        virtual ~IRtcpReceiver() = default;

        /* Parse a compound RTCP packet and notify the observer per sub-packet. */
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
        /* Generate periodic RTCP packet(s), such as RR/SR/SDES. */
        virtual bool GenerateRtcp(uint32_t media_ssrc, std::vector<uint8_t>& out) = 0;

        /* Build a PSFB PLI packet (PT=206, FMT=1). Returns bytes to send. */
        virtual std::vector<uint8_t> BuildPli(uint32_t sender_ssrc,
                                        uint32_t media_ssrc) = 0;

        std::unique_ptr<IRtcpReceiver> CreateRtcpReceiver();
        std::unique_ptr<IRtcpSender>   CreateRtcpSender(); 
    };

}


#endif /* _RTCPCONTEXT_H_ */
