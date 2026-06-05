#ifndef _RTPRECEIVER_H_
#define _RTPRECEIVER_H_


#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "RtpTypes.h"
#include "Rtp.h"
#include "RtcpReciver.h"
#include "H264Depacketizer.h"
#include "logger.h"

class Frame;
using FramePtr = std::shared_ptr<Frame>;


class RtpReceiverTrack : public EnhancedPacketSortor<RtpPacket::Ptr, uint16_t>
{
public:
    using Ptr = std::shared_ptr<RtpReceiverTrack>;
    using FrameCallback = std::function<void(const FramePtr &)>;
    using NackCallback = std::function<void(const uint16_t *seqs, size_t count)>;
    using PliCallback = std::function<void()>;
    using FirCallback = std::function<void(uint8_t seq_nr)>;

public:
    explicit RtpReceiverTrack(const TrackInfo &info)
        : _info(info)
    {
        setOnPacketSorted([this](uint16_t seq, const RtpPacket::Ptr &pkt) {
            (void)seq;
            this->onBeforeRtpSorted(pkt);
            this->onRtpSorted(pkt);
            ++_stats.sorted_packets;
            _stats.last_seq = pkt ? pkt->getSeq() : _stats.last_seq;
            _stats.last_ts = pkt ? pkt->getStamp() : _stats.last_ts;
        });
    }

    virtual ~RtpReceiverTrack() = default;

public:
    const TrackInfo &getTrackInfo() const { return _info; }

    TrackType getTrackType() const { return _info.type; }
    CodecId getCodecId() const { return _info.codec_id; }
    const std::string &getCodecName() const { return _info.codec_name; }

    uint32_t getSSRC() const { return _info.ssrc; }
    uint8_t getPayloadType() const { return _info.payload_type; }
    int getTrackIndex() const { return _info.track_index; }

    void setSSRC(uint32_t ssrc) { _info.ssrc = ssrc; }
    void setPayloadType(uint8_t pt) { _info.payload_type = pt; }
    void setTrackIndex(int index) { _info.track_index = index; }

    void setRtspTransport(const RtspTransport &transport) { _rtsp_transport = transport; }
    const RtspTransport &getRtspTransport() const { return _rtsp_transport; }

    void setOnFrame(FrameCallback cb) { _on_frame = std::move(cb); }

    const RtpTrackStats &getStats() const { return _stats; }


public:
    virtual RtpPacket::Ptr inputRtp(TrackType type, int sample_rate, uint8_t *ptr, size_t len) = 0;
    virtual void inputRtcp(const uint8_t *ptr, size_t len) = 0;
protected:
    bool inputPacket(const RtpPacket::Ptr &pkt)
    {
        if (!pkt) 
        {
            return false;
        }
        ++_stats.received_packets;
        _stats.seen_packet = true;
        EnhancedPacketSortor<RtpPacket::Ptr, uint16_t>::inputPacket(pkt->getSeq(), pkt);
        return true;
    }

    bool emitFrame(const FramePtr &frame)
    {
        if (_on_frame) 
        {
            _on_frame(frame);
            return true;
        }
        return false;
    }

    virtual void onBeforeRtpSorted(const RtpPacket::Ptr &pkt)
    {

    }
    virtual void onRtpSorted(const RtpPacket::Ptr &pkt)
    {
        
    }

protected:
    TrackInfo _info;
    RtspTransport _rtsp_transport;
    RtpTrackStats _stats;

    FrameCallback _on_frame;
};


class RtpVideoTracker : public RtpReceiverTrack, public RtpRecvStatsBase
{
public:
    using Ptr = std::shared_ptr<RtpVideoTracker>;

    explicit RtpVideoTracker(const TrackInfo& info)
        : RtpReceiverTrack(info)
        , _depacketizer(std::make_unique<H264Depacketizer>())
    {
    }

    ~RtpVideoTracker() override = default;

public:
    RtpPacket::Ptr inputRtp(TrackType type, int sample_rate, uint8_t* ptr, size_t len) override;

    void OnRtcpSenderReport(uint32_t sender_ssrc, uint64_t ntp, uint32_t rtp_ts, uint32_t packet_count,uint32_t octet_count)
    {
        RtpRecvStatsBase::OnSenderReport(sender_ssrc, ntp, rtp_ts, packet_count, octet_count);
    }

    void OnRtcpBye(uint32_t sender_ssrc)
    {
        LOG_INFO("[RTCP][BYE]", " sender_ssrc=", sender_ssrc);
        CountBye();
    }

protected:
    void onBeforeRtpSorted(const RtpPacket::Ptr& pkt) override;
    void onRtpSorted(const RtpPacket::Ptr& pkt) override;

private:
    std::unique_ptr<Depacketizer> _depacketizer;
};

/**
 * @brief RTCP event dispatcher.
 *
 * RtcpDispatcher receives parsed RTCP events from RtcpReceiverImpl and
 * dispatches them to the corresponding RTP receiver/sender track according
 * to SSRC and RTCP packet semantics.
 *
 * Design:
 * - Sender Report(SR) is usually sent by the remote RTP sender, so it should
 *   be dispatched to the receiver track that is receiving this media SSRC.
 *
 * - Receiver Report(RR), NACK, PLI and FIR are feedback for packets sent by
 *   the local sender track, so they should be dispatched to the sender track
 *   whose RTP SSRC matches the media_ssrc in the RTCP packet.
 *
 * Typical RTCP routing:
 * - SR  -> RtpReceiverTrack / RtpRecvStatsBase
 * - RR  -> RtpSenderTrack / RtpSendStatsBase
 * - NACK -> RtpSenderTrack, retransmit from RTP packet cache
 * - PLI/FIR -> RtpSenderTrack or upper session, request key frame
 */
class RtcpDispatcher : public rtcpx::IRtcpObserver
{
public:
    /**
     * @brief Add a receiver track for RTCP dispatching.
     *
     * Receiver tracks are used for RTCP packets that describe or control
     * the remote RTP source being received, such as Sender Report(SR).
     *
     * @param media_ssrc RTP media SSRC received by this receiver track.
     * @param track Receiver track instance.
     */
    void AddReceiverTrack(uint32_t media_ssrc, std::weak_ptr<RtpReceiverTrack> track);

    /**
     * @brief Add a sender track for RTCP feedback dispatching.
     *
     * Sender tracks are used for RTCP feedback packets targeting locally
     * sent RTP streams, such as RR, NACK, PLI and FIR.
     *
     * @param media_ssrc Local RTP media SSRC sent by this sender track.
     * @param track Sender track instance.
     */
    void AddSenderTrack(uint32_t media_ssrc, std::weak_ptr<RtpSenderTrack> track);

     /**
     * @brief Handle RTCP Sender Report.
     *
     * Sender Report is generated by the RTP sender. For a receiving track,
     * it provides the mapping between NTP timestamp and RTP timestamp,
     * and also carries sender packet/octet statistics.
     *
     * Dispatch target:
     * - recv_tracks_[sender_ssrc]
     *
     * @param sender_ssrc SSRC of the RTP sender.
     * @param ntp NTP timestamp from SR.
     * @param rtp_ts RTP timestamp corresponding to the NTP timestamp.
     * @param packet_count Total RTP packet count sent by sender.
     * @param octet_count Total RTP payload octet count sent by sender.
     */
    void OnSenderReport(uint32_t sender_ssrc, uint64_t ntp, uint32_t rtp_ts, uint32_t packet_count, uint32_t octet_count) override;
    
    /**
     * @brief Handle RTCP Receiver Report.
     *
     * Receiver Report is feedback from the remote receiver about the RTP
     * stream sent by local side. It contains packet loss, highest sequence,
     * jitter and RTT-related fields.
     *
     * Dispatch target:
     * - send_tracks_[media_ssrc]
     *
     * @param reporter_ssrc SSRC of the RTCP reporter.
     * @param media_ssrc SSRC of the RTP stream being reported.
     * @param fraction_lost Recent packet loss ratio, fixed 8-bit fraction.
     * @param cumulative_lost Total number of RTP packets lost, signed 24-bit.
     * @param highest_seq Extended highest RTP sequence number received.
     * @param jitter Interarrival jitter.
     * @param lsr Last Sender Report timestamp.
     * @param dlsr Delay since last Sender Report.
     */
    void OnReceiverReport(uint32_t reporter_ssrc, uint32_t media_ssrc, uint8_t fraction_lost, int32_t cumulative_lost, uint32_t highest_seq, uint32_t jitter, uint32_t lsr, uint32_t dlsr) override;

    /**
     * @brief Handle RTCP Generic NACK feedback.
     *
     * NACK means the remote receiver has detected packet loss and requests
     * retransmission of specific RTP sequence numbers.
     *
     * Dispatch target:
     * - send_tracks_[media_ssrc]
     *
     * The target sender track should look up the RTP packet cache and
     * retransmit the requested packets if still available.
     *
     * @param sender_ssrc SSRC of the RTCP feedback sender.
     * @param media_ssrc SSRC of the RTP stream that lost packets.
     * @param seqs Lost RTP sequence numbers expanded from PID/BLP pairs.
     * @param count Number of sequence numbers in seqs.
     */
    void OnNack(uint32_t sender_ssrc, uint32_t media_ssrc, const uint16_t* seqs, size_t count) override;

    /**
     * @brief Handle RTCP Picture Loss Indication.
     *
     * PLI means the remote decoder cannot correctly decode the current video
     * stream and requests a new key frame.
     *
     * Dispatch target:
     * - send_tracks_[media_ssrc]
     *
     * In SFU mode, this event may also need to be forwarded to the upstream
     * publisher instead of directly triggering a local encoder.
     *
     * @param sender_ssrc SSRC of the RTCP feedback sender.
     * @param media_ssrc SSRC of the RTP video stream that needs a key frame.
     */
    void OnPli(uint32_t sender_ssrc, uint32_t media_ssrc) override;

    /**
     * @brief Handle RTCP Full Intra Request.
     *
     * FIR is also a key-frame request, but its semantics are stronger than
     * PLI. It requests a complete intra refresh frame.
     *
     * Dispatch target:
     * - send_tracks_[media_ssrc]
     *
     * @param sender_ssrc SSRC of the RTCP feedback sender.
     * @param media_ssrc SSRC of the RTP video stream that needs a key frame.
     * @param seq_nr FIR command sequence number.
     */
    void OnFir(uint32_t sender_ssrc, uint32_t media_ssrc, uint8_t seq_nr) override;

private:
    std::unordered_map<uint32_t, std::weak_ptr<RtpReceiverTrack>> recv_tracks_;
    std::unordered_map<uint32_t, std::weak_ptr<RtpSenderTrack>> send_tracks_;
};

#endif // _RTPRECEIVER_H_
