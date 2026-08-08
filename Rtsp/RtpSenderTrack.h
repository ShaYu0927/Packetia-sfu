#ifndef _RTP_SENDER_TRACKER_H_
#define _RTP_SENDER_TRACKER_H_

#include <cstdint>
#include <cstddef>
#include <vector>
#include <deque>
#include <unordered_map>
#include <functional>
#include <memory>
#include "IMediaTransport.h"
#include "Rtp.h"

namespace rtsp
{
struct RtpSenderTrackConfig
{
    /*
     * SSRC used by this sender track when sending RTP packets to the downstream peer.
     *
     * In an SFU/forwarding scenario, this SSRC is usually generated locally for
     * each subscriber and does not have to be the same as the upstream RTP SSRC.
     */
    uint32_t local_ssrc = 0;

    /*
     * Whether to rewrite the RTP payload type.
     *
     * false:
     *   Keep the payload type from the upstream RTP packet.
     *
     * true:
     *   Rewrite the payload type field in the RTP header to `payload_type`.
     */
    bool rewrite_payload_type = false;

    /*
     * RTP payload type used when `rewrite_payload_type` is enabled.
     *
     * Common dynamic payload types are in the range 96~127, for example H264/H265.
     */
    uint8_t payload_type = 96;

    /*
     * RTP timestamp clock rate.
     *
     * Common values:
     *   Video      : 90000
     *   Opus       : 48000
     *   PCMA/PCMU  : 8000
     *
     * For RTP packet forwarding, this field is mainly used later for RTCP Sender
     * Report, A/V synchronization, bitrate calculation, and statistics.
     */
    int sample_rate = 90000;

    /*
     * Number of RTP packets kept in the retransmission cache.
     *
     * This cache is used to handle downstream RTCP NACK feedback. When the
     * downstream peer reports lost sequence numbers, RtpSenderTrack can find
     * the corresponding RTP packets from this cache and retransmit them.
     *
     * Suggested values:
     *   0     : Disable RTP cache and NACK retransmission.
     *   512   : Default value, suitable for the first implementation.
     *   1024+ : Useful for high bitrate or weak network scenarios.
     */
    size_t rtp_cache_size = 512;

    /*
     * Minimum interval between two retransmissions of the same RTP packet.
     */
    uint64_t min_retransmit_interval_ms = 30;

    /*
     * Maximum retransmission attempts per cached RTP packet.
     */
    uint32_t max_retransmit_count = 3;
};

class RtpSenderTrack
{
public:
    using Ptr = std::shared_ptr<RtpSenderTrack>;
    using KeyFrameRequestCallback = std::function<void()>;
    using PacketSentCallback = std::function<void(uint16_t transport_sequence,
                                                  uint32_t ssrc,
                                                  uint16_t rtp_sequence,
                                                  uint64_t send_time_ms,
                                                  uint32_t size_bytes)>;

public:
    RtpSenderTrack(const RtpSenderTrackConfig& config, std::shared_ptr<IMediaTransport> transport);

    ~RtpSenderTrack() = default;

public:
    bool InputRtpPacket(const uint8_t* data, size_t len);
    void OnRtcpNack(const std::vector<uint16_t>& lost_seqs);

    void OnRtcpReceiverReport(uint32_t reporter_ssrc,
                              uint32_t media_ssrc,
                              uint8_t fraction_lost,
                              int32_t cumulative_lost,
                              uint32_t highest_seq,
                              uint32_t jitter,
                              uint32_t lsr,
                              uint32_t dlsr);

    void OnRtcpPli();

    void OnRtcpFir();

    void Tick(uint64_t now_ms);

    void SetKeyFrameRequestCallback(KeyFrameRequestCallback cb);
    void SetPacketSentCallback(PacketSentCallback cb);

    uint32_t GetSsrc() const { return _config.local_ssrc; }

    uint64_t GetPacketCount() const { return _packet_count; }

    uint64_t GetOctetCount() const { return _octet_count; }

    uint64_t GetReceiverReportCount() const { return _rr_count; }

    uint8_t GetLastRrFractionLost() const { return _last_rr_fraction_lost; }

    int32_t GetLastRrCumulativeLost() const { return _last_rr_cumulative_lost; }

    uint32_t GetLastRrHighestSeq() const { return _last_rr_highest_seq; }

    uint32_t GetLastRrJitter() const { return _last_rr_jitter; }

    uint32_t GetRttMs() const { return _rtt_ms; }


private:
    bool ParseRtpHeader(const uint8_t* data, size_t len, RtpHeader& header);

    bool RewriteRtpPacket(std::vector<uint8_t>& packet, const RtpHeader& in_header, uint16_t& out_seq, uint32_t& out_timestamp);

    uint16_t RewriteSeq(uint16_t in_seq);

    uint32_t RewriteTimestamp(uint32_t in_timestamp);

    void CacheRtpPacket(uint16_t out_seq, const std::vector<uint8_t>& packet);

    bool SendRtpPacket(const std::vector<uint8_t>& packet,
                       bool retransmit = false);

private:
    struct CachedRtpPacket
    {
        std::vector<uint8_t> packet;
        uint64_t last_retransmit_ms = 0;
        uint32_t retransmit_count = 0;
    };

     /*
     * Sender track configuration.
     *
     * It describes the downstream SSRC, payload type rewrite policy,
     * RTP clock rate, retransmission cache size, and the source track info.
     */
    RtpSenderTrackConfig _config;

    /*
     * Underlying media transport.
     *
     * The session owns the transport. Keeping a weak reference prevents this
     * track from extending the connection lifetime and avoids dangling raw
     * pointers when the session is closed first.
     */
    std::weak_ptr<IMediaTransport> _transport;

    /*
     * Whether the first RTP packet has been received.
     *
     * The first RTP packet is used to initialize sequence number and timestamp
     * rewrite offsets.
     */
    bool _started = false;

    /*
     * Base sequence number from the upstream RTP stream.
     *
     * Used as the reference point for downstream sequence number rewriting.
     */
    uint16_t _base_in_seq = 0;

    /*
     * Base sequence number generated for the downstream RTP stream.
     *
     * Downstream sequence number is calculated as:
     *   out_seq = _base_out_seq + (in_seq - _base_in_seq)
     */
    uint16_t _base_out_seq = 0;

    /*
     * Base RTP timestamp from the upstream RTP stream.
     *
     * Used as the reference point for downstream timestamp rewriting.
     */
    uint32_t _base_in_timestamp = 0;

    /*
     * Base RTP timestamp generated for the downstream RTP stream.
     *
     * Downstream timestamp is calculated as:
     *   out_timestamp = _base_out_timestamp + (in_timestamp - _base_in_timestamp)
     */
    uint32_t _base_out_timestamp = 0;

    /*
     * Number of RTP packets successfully sent by this sender track.
     *
     * This value can be used later for RTCP Sender Report.
     */
    uint64_t _packet_count = 0;

    /*
     * Number of RTP payload bytes or packet bytes sent by this sender track.
     *
     * Note:
     *   For strict RTCP Sender Report, this should usually count RTP payload
     *   octets, not the full RTP packet size.
     */
    uint64_t _octet_count = 0;

    /*
     * Last tick timestamp in milliseconds.
     *
     * Used by periodic tasks such as RTCP Sender Report, pacer driving,
     * statistics update, or congestion control.
     */
    uint64_t _last_tick_ms = 0;

    uint64_t _rr_count = 0;                  // Number of RR packets received.
    uint32_t _last_rr_reporter_ssrc = 0;     // SSRC of the sender that generated the latest RR.
    uint32_t _last_rr_media_ssrc = 0;        // SSRC of the media stream reported in the latest RR.
    uint8_t  _last_rr_fraction_lost = 0;     // Fraction of RTP packets lost since the previous RR (8-bit fixed point).
    int32_t  _last_rr_cumulative_lost = 0;   // Total number of RTP packets lost since the beginning of reception.
    uint32_t _last_rr_highest_seq = 0;       // Highest extended RTP sequence number received by the reporter.
    uint32_t _last_rr_jitter = 0;            // Estimated RTP packet inter-arrival jitter reported by RR.
    uint32_t _last_rr_lsr = 0;               // NTP timestamp middle 32 bits from the last Sender Report (for RTT calculation).
    uint32_t _last_rr_dlsr = 0;              // Delay since last Sender Report, used together with LSR to calculate RTT.
    uint32_t _rtt_ms = 0;                    // Estimated round-trip time in milliseconds.

    /*
     * Last time a PLI/FIR key frame request was forwarded upstream.
     *
     * Used to avoid sending key frame requests too frequently.
     */
    uint64_t _last_pli_ms = 0;

    /*
     * Minimum interval between two upstream key frame requests, in milliseconds.
     *
     * This prevents PLI/FIR storms when the downstream network is unstable.
     */
    uint64_t _pli_interval_ms = 1000;

    /*
     * RTP retransmission cache order.
     *
     * Stores downstream sequence numbers in insertion order so that old packets
     * can be removed when the cache exceeds the configured size.
     */
    std::deque<uint16_t> _cache_order;

    /*
     * RTP retransmission cache.
     *
     * Key:
     *   Downstream RTP sequence number after rewriting.
     *
     * Value:
     *   Cached RTP packet used for RTCP NACK retransmission.
     */
    std::unordered_map<uint16_t, CachedRtpPacket> _rtp_cache;

    /*
     * Callback used to request an upstream key frame.
     *
     * It is triggered when the downstream peer sends RTCP PLI or FIR.
     * Usually only meaningful for video tracks.
     */
    KeyFrameRequestCallback _keyframe_cb;
    PacketSentCallback      _packet_sent_cb;
};



}


#endif /* _RTP_SENDER_TRACKER_H_ */
