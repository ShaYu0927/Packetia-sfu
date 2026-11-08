#ifndef _RTP_SENDER_TRACKER_H_
#define _RTP_SENDER_TRACKER_H_

#include "SharedBuffer.h"
#include "PoolAllocator.h"

#include <cstdint>
#include <cstddef>
#include <vector>
#include <deque>
#include <unordered_map>
#include <functional>
#include <memory>
#include <string>
#include "IMediaTransport.h"
#include "Rtp.h"
#include "TransportSequenceAllocator.h"
#include "NetworkStats.h"
#include "Rtp/RtpRetransmissionCache.h"
#include "Rtp/RtpForwardingState.h"
#include "Rtp/RtpSenderFeedback.h"

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

    /*
     * SDP 协商得到的 Transport-CC RTP Header Extension ID。
     * 0 表示禁用；当前写入器支持 RFC 8285 one-byte ID 1~14。
     */
    uint8_t transport_cc_extension_id = 0;

    // Rebuild extensions for a different RTP session. Drop upstream extension
    // IDs, write the downstream MID, then let PrepareTransportCc append TWCC.
    bool rewrite_header_extensions = false;
    uint8_t mid_extension_id = 0;
    std::string mid;

    /*
     * 当前下行 Transport 共享的 TWCC 分配器。音频、视频和 RTX Track
     * 必须注入同一个实例，不能每个 Track 单独创建。
     * Transport 提供 SendSideController 时自动使用它的 allocator；
     * 本字段仅作为自定义 Transport 未提供控制器时的依赖注入入口。
     */
    std::shared_ptr<media::TransportSequenceAllocator> transport_sequence_allocator;

    // Maximum age since the original send was accepted. 0 disables expiry;
    // tune together with the receiver's recovery budget, not the cache size.
    uint64_t max_retransmit_age_ms = 500;

    // RTX is disabled until the peer and sender support it.
    bool rtx_enabled = false;
    uint32_t rtx_ssrc = 0;
    uint8_t rtx_payload_type = 0;
    uint16_t initial_rtx_sequence = 0;
};

class RtpSenderTrack
{
public:
    using Ptr = std::shared_ptr<RtpSenderTrack>;
    using Clock = std::function<uint64_t()>;
    using KeyFrameRequestCallback = std::function<void()>;
    using PacketSentCallback = std::function<void(int64_t extended_transport_sequence,
                                                  uint16_t wire_transport_sequence,
                                                  uint32_t ssrc,
                                                  uint16_t rtp_sequence,
                                                  uint64_t send_time_ms,
                                                  uint32_t size_bytes)>;

public:
    RtpSenderTrack(const RtpSenderTrackConfig& config, std::shared_ptr<IMediaTransport> transport,
                   Clock clock = {});

    ~RtpSenderTrack();
    RtpSenderTrack(const RtpSenderTrack&) = delete;
    RtpSenderTrack& operator=(const RtpSenderTrack&) = delete;

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
    // Start a new forwarding epoch after intentional packet suppression. The
    // first recovery packet follows the last sent RTP sequence, including pads.
    void BeginDiscontinuity();
    bool SendPadding(size_t padding_bytes, int probe_cluster_id = -1);

    uint32_t GetSsrc() const { return _config.local_ssrc; }

    uint64_t GetPacketCount() const { return _packet_count; }

    uint64_t GetOctetCount() const { return _octet_count; }

    uint64_t GetReceiverReportCount() const { return _feedback.rr_count; }

    uint8_t GetLastRrFractionLost() const { return _feedback.fraction_lost; }

    int32_t GetLastRrCumulativeLost() const { return _feedback.cumulative_lost; }

    uint32_t GetLastRrHighestSeq() const { return _feedback.highest_seq; }

    uint32_t GetLastRrJitter() const { return _feedback.jitter; }

    uint32_t GetRttMs() const { return _feedback.rtt_ms; }


private:
    bool ParseRtpHeader(const uint8_t* data, size_t len, RtpHeader& header);

    bool RewriteRtpPacket(common::ByteVector& packet, const RtpHeader& in_header, uint16_t& out_seq, uint32_t& out_timestamp);
    bool RewriteHeaderExtensions(common::ByteVector& packet);

    bool WriteTransportCcExtension(common::ByteVector& packet,
                                   uint16_t transport_sequence) const;

    bool PrepareTransportCc(common::ByteVector& packet,
                            media::TransportSequenceNumber& sequence) const;

    bool SendRtpPacket(const common::ByteVector& packet, bool retransmit,
                       const media::TransportSequenceNumber& sequence, uint16_t rtp_sequence,
                       int probe_cluster_id = -1, media::MediaKind kind = media::MediaKind::Unknown);

    bool BuildRtxPacket(const uint8_t* original_data, size_t original_size,  uint16_t rtx_sequence, common::ByteVector& output) const;

private:
     /*
     * Sender track configuration.
     *
     * It describes the downstream SSRC, payload type rewrite policy,
     * RTP clock rate, retransmission cache size, and the source track info.
     */
    RtpSenderTrackConfig _config;
    Clock _clock;
    uint16_t _next_rtx_sequence;
    RtpRetransmissionCache _retransmission_cache;
    RtpForwardingState _forwarding;

    /*
     * Underlying media transport.
     *
     * The session owns the transport. Keeping a weak reference prevents this
     * track from extending the connection lifetime and avoids dangling raw
     * pointers when the session is closed first.
     */
    std::weak_ptr<IMediaTransport> _transport;
    std::shared_ptr<media::SendSideController> _send_controller;

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

    RtpSenderFeedback _feedback;

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
