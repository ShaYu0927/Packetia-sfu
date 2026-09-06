#ifndef _RTPRECEIVER_H_
#define _RTPRECEIVER_H_


#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "RtpTypes.h"
#include "Rtp.h"
#include "RtcpReciver.h"
#include "TrackClock.h"
#include "RtpSenderTrack.h"
#include "H264Depacketizer.h"
#include "logger.h"
#include "AudioDepacketizer.h"
#include "AudioRtpDepacketizerFactory.h"
#include "MediaFrame.h"
#include "NackRequester.h"

namespace rtsp 
{

class RtpPacketPool : public std::enable_shared_from_this<RtpPacketPool>
{
public:
    using Ptr = std::shared_ptr<RtpPacketPool>;

    explicit RtpPacketPool(size_t capacity, size_t packet_capacity = RtpPacket::kRtpMaxSize)
    {
        packets_.reserve(capacity);
        free_.reserve(capacity);
        for (size_t i = 0; i < capacity; ++i)
        {
            auto packet = std::make_unique<RtpPacket>();
            if (!packet->reserve(packet_capacity))
            {
                continue;
            }
            free_.push_back(packet.get());
            packets_.push_back(std::move(packet));
        }
    }

    RtpPacket::Ptr Acquire()
    {
        RtpPacket* packet = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (free_.empty())
            {
                return nullptr;
            }
            packet = free_.back();
            free_.pop_back();
        }

        auto self = shared_from_this();
        return RtpPacket::Ptr(packet, [self](RtpPacket* item) {
            self->Release(item);
        });
    }

    size_t Capacity() const noexcept
    {
        return packets_.size();
    }

    size_t Available() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return free_.size();
    }

private:
    void Release(RtpPacket* packet)
    {
        if (!packet)
        {
            return;
        }

        packet->resetForReuse();
        std::lock_guard<std::mutex> lock(mutex_);
        free_.push_back(packet);
    }

    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<RtpPacket>> packets_;
    std::vector<RtpPacket*> free_;
};


/**
 * @brief Base class for receiving RTP packets of one media track.
 *
 * RtpReceiverTrack is responsible for:
 *   1. Receiving raw RTP packets from the transport layer.
 *   2. Parsing RTP packets in derived classes.
 *   3. Sorting RTP packets by sequence number.
 *   4. Delivering ordered RTP packets to depacketizers.
 *   5. Emitting decoded media frames through frame callbacks.
 *
 * This class does not implement codec-specific depacketization.
 * Codec-specific logic should be implemented by derived classes, such as:
 *   - RtpVideoTracker
 *   - RtpAudioTracker
 */
class RtpReceiverTrack : public EnhancedPacketSortor<RtpPacket::Ptr, uint16_t>,
                         public RtpRecvStatsBase
{
public:
    using Ptr = std::shared_ptr<RtpReceiverTrack>;
    using EncodedFramePtr = media::EncodedFrame::Ptr;

    /**
     * @brief Callback used when a complete media frame is generated.
     *
     * For video, this may be a complete H264/H265 frame.
     * For audio, this may be one audio access unit or audio frame.
     */
    using FrameCallback = std::function<void(const FramePtr &)>;
    using EncodedFrameCallback = std::function<void(const EncodedFramePtr&)>;

    /**
     * @brief Callback used to report lost RTP sequence numbers.
     *
     * This callback can be used to generate RTCP NACK feedback.
     */
    using NackCallback = std::function<void(const uint16_t *seqs, size_t count)>;

    /**
     * @brief Callback used to request a key frame through RTCP PLI.
     */
    using PliCallback = std::function<void()>;

    /**
     * @brief Callback used to request a key frame through RTCP FIR.
     *
     * seq_nr is the FIR command sequence number.
     */
    using FirCallback = std::function<void(uint8_t seq_nr)>;

    using SendNackCallback = std::function<void(uint32_t ssrc, const std::vector<uint16_t>& lost_seq)>;
    using NackRecoveryFailureCallback = std::function<void(uint32_t ssrc, const std::vector<uint16_t>& abandoned_seq)>;

public:
    static Ptr Create(const TrackInfo& info);

    /**
     * @brief Construct a receiver track with basic track information.
     *
     * The packet sorter callback is registered here. Once a packet becomes
     * ordered by sequence number, the following steps are executed:
     *   1. onBeforeRtpSorted()
     *   2. onRtpSorted()
     *   3. Update receive statistics
     *
     * @param info Track metadata, including media type, codec, payload type,
     *             SSRC, and track index.
     */
    explicit RtpReceiverTrack(const TrackInfo &info)
        : _info(info),
          packet_pool_(std::make_shared<RtpPacketPool>(kPacketPoolCapacity)),
          track_clock_(info.clock_rate > 0
                           ? static_cast<uint32_t>(info.clock_rate)
                           : (info.type == TrackVideo ? 90000U : 0U))
    {
        setOnPacketSorted([this](uint16_t seq, const RtpPacket::Ptr &pkt) {
            (void)seq;

            /*
             * Hook before handling a sorted RTP packet.
             *
             * Derived classes may override this to update statistics,
             * jitter information, or RTCP receive-side state before
             * depacketization.
             */
            this->onBeforeRtpSorted(pkt);

            /*
             * Handle an ordered RTP packet.
             *
             * Derived classes usually perform codec-specific depacketization
             * here and emit complete media frames when available.
             */
            this->onRtpSorted(pkt);

            /*
             * Update common receiver statistics after the packet has been
             * accepted by the sorter.
             */
            ++_stats.sorted_packets;
            _stats.last_seq = pkt ? pkt->getSeq() : _stats.last_seq;
            _stats.last_ts = pkt ? pkt->getStamp() : _stats.last_ts;
        });

    }

    virtual ~RtpReceiverTrack() = default;

public:
    /**
     * @brief Get immutable track metadata.
     */
    const TrackInfo &getTrackInfo() const { return _info; }

    /**
     * @brief Get media track type, such as audio or video.
     */
    TrackType getTrackType() const { return _info.type; }

    /**
     * @brief Get codec identifier.
     */
    CodecId getCodecId() const { return _info.codec_id; }

    /**
     * @brief Get codec name string.
     */
    const std::string &getCodecName() const { return _info.codec_name; }

    /**
     * @brief Get RTP SSRC of this receiving track.
     */
    uint32_t getSSRC() const { return _info.ssrc; }

    /**
     * @brief Get RTP payload type of this track.
     */
    uint8_t getPayloadType() const { return _info.payload_type; }

    /**
     * @brief Get media track index.
     *
     * For example:
     *   0 may represent video.
     *   1 may represent audio.
     */
    int getTrackIndex() const { return _info.track_index; }

    /**
     * @brief Set RTP SSRC for this receiving track.
     */
    void setSSRC(uint32_t ssrc) { _info.ssrc = ssrc; }

    /**
     * @brief Set RTP payload type for this receiving track.
     */
    void setPayloadType(uint8_t pt) { _info.payload_type = pt; }

    /**
     * @brief Set media track index.
     */
    void setTrackIndex(int index) { _info.track_index = index; }

    /**
     * @brief Set RTSP transport information.
     *
     * This is mainly used in RTSP scenarios, where RTP may be transported
     * over UDP or interleaved TCP.
     */
    void setRtspTransport(const RtspTransport &transport) { _rtsp_transport = transport; }

    /**
     * @brief Get RTSP transport information.
     */
    const RtspTransport &getRtspTransport() const { return _rtsp_transport; }

    /**
     * @brief Set callback for receiving complete media frames.
     *
     * Derived classes should call emitFrame() after depacketizing RTP packets
     * into complete media frames.
     */
    void setOnFrame(FrameCallback cb) { _on_frame = std::move(cb); }
    void setOnEncodedFrame(EncodedFrameCallback cb) { _on_encoded_frame = std::move(cb); }

    /**
     * @brief Get RTP receiving statistics.
     */
    const RtpTrackStats &getStats() const { return _stats; }

public:
    /**
     * @brief Input one raw RTP packet.
     *
     * Derived classes should implement:
     *   1. RTP packet parsing.
     *   2. RTP header validation.
     *   3. Track matching if necessary.
     *   4. Calling inputPacket() after a valid RtpPacket is created.
     *
     * @param ptr         Raw RTP packet buffer.
     * @param len         Raw RTP packet length.
     *
     * @return Parsed RtpPacket on success, nullptr on failure.
     */
    virtual RtpPacket::Ptr inputRtp(uint8_t *ptr, size_t len) = 0;

    virtual void OnRtcpSenderReport(uint32_t sender_ssrc, uint64_t ntp, uint32_t rtp_ts, uint32_t packet_count, uint32_t octet_count)
    {
        track_clock_.UpdateSenderReport(ntp, rtp_ts);
        RtpRecvStatsBase::OnSenderReport(
            sender_ssrc, ntp, rtp_ts, packet_count, octet_count,
            NowMs(), track_clock_.ClockRate());
    }

    virtual void OnRtcpBye(uint32_t sender_ssrc)
    {
        (void)sender_ssrc;
    }

protected:
    bool recordPacketReceived(const RtpPacket::Ptr& pkt)
    {
        if (!pkt)
            return false;
        ++_stats.received_packets;
        _stats.seen_packet = true;
        OnRtpPacket(pkt->getSSRC(), pkt->getPayloadType(), pkt->getSeq(),
                    pkt->getStamp(), pkt->getPayloadSize(), pkt->getRecvTimeMs(),
                    pkt->getSampleRate());
        return true;
    }

    /**
     * @brief Input a parsed RTP packet into the packet sorter.
     *
     * This function updates common receive statistics and then passes the
     * packet to EnhancedPacketSortor. The sorter will reorder packets by RTP
     * sequence number and call onRtpSorted() when packets become ordered.
     *
     * @param pkt Parsed RTP packet.
     *
     * @return true if the packet is accepted, false otherwise.
     */
    bool inputPacket(const RtpPacket::Ptr &pkt)
    {
        if (!recordPacketReceived(pkt))
            return false;
        EnhancedPacketSortor<RtpPacket::Ptr, uint16_t>::inputPacket(pkt->getSeq(), pkt);

        return true;
    }

    // Video packet buffers need to observe packets in arrival order so a
    // retransmitted gap can unlock frames already buffered behind it.
    bool inputUnorderedPacket(const RtpPacket::Ptr& pkt)
    {
        if (!recordPacketReceived(pkt))
            return false;
        onRtpSorted(pkt);
        ++_stats.sorted_packets;
        _stats.last_seq = pkt->getSeq();
        _stats.last_ts = pkt->getStamp();
        return true;
    }

    RtpPacket::Ptr acquirePacket(size_t packet_size)
    {
        EnhancedPacketSortor<RtpPacket::Ptr, uint16_t>::flushExpired();

        if (packet_size > RtpPacket::kRtpMaxSize)
        {
            ++_stats.oversized_packets;
            return nullptr;
        }

        auto packet = packet_pool_ ? packet_pool_->Acquire() : nullptr;
        if (!packet)
        {
            ++_stats.pool_exhausted_packets;
        }
        return packet;
    }

    /**
     * @brief Emit a complete media frame to the upper layer.
     *
     * This is usually called by derived classes after depacketization.
     *
     * @param frame Complete media frame.
     *
     * @return true if a frame callback exists and is called, false otherwise.
     */
    bool emitFrame(const FramePtr &frame)
    {
        if (_on_frame)
        {
            _on_frame(frame);
            return true;
        }

        return false;
    }

    bool emitEncodedFrame(const EncodedFramePtr& frame)
    {
        if (frame)
        {
            if (frame->info.timestamp.receive_time_ms == 0)
            {
                frame->info.timestamp.receive_time_ms = frame->info.timestamp.capture_time_ms;
            }

            const auto mapped = track_clock_.Map(frame->rtp.rtp_timestamp);
            if (mapped.valid)
            {
                frame->info.timestamp.capture_time_ms = mapped.unix_time_us / 1000;
                frame->info.timestamp.capture_time_valid = true;
            }
            else if (frame->info.timestamp.capture_time_ms == 0)
            {
                // Preserve the previous arrival-time fallback until the first
                // valid SR establishes a sender clock mapping.
                frame->info.timestamp.capture_time_ms = frame->info.timestamp.receive_time_ms;
            }
        }

        if (_on_encoded_frame)
        {
            _on_encoded_frame(frame);
            return true;
        }

        return false;
    }

    /**
     * @brief Hook called before handling a sorted RTP packet.
     *
     * Derived classes can override this function to update receive-side
     * statistics, jitter calculation, RTCP state, or debugging information.
     *
     * Default implementation does nothing.
     */
    virtual void onBeforeRtpSorted(const RtpPacket::Ptr &pkt)
    {
        (void)pkt;
    }

    /**
     * @brief Handle one ordered RTP packet.
     *
     * Derived classes should override this function to perform codec-specific
     * depacketization.
     *
     * For example:
     *   - H264 FU-A/STAP-A depacketization
     *   - H265 AP/FU depacketization
     *   - AAC RTP payload parsing
     *
     * Default implementation does nothing.
     */
    virtual void onRtpSorted(const RtpPacket::Ptr &pkt)
    {
        (void)pkt;
    }

public:
    void setSendNackCallback(SendNackCallback cb) { _on_send_nack = std::move(cb);}
    void setNackRecoveryFailureCallback(NackRecoveryFailureCallback cb) { _on_nack_recovery_failure = std::move(cb); }

protected:
    static constexpr size_t kPacketPoolCapacity = 64;

    /**
     * @brief Track metadata.
     *
     * Includes track type, codec id, codec name, SSRC, payload type,
     * and track index.
     */
    TrackInfo _info;
    RtpPacketPool::Ptr packet_pool_;

    /**
     * @brief RTSP transport information.
     *
     * Used when this RTP receiver track belongs to an RTSP session.
     */
    RtspTransport _rtsp_transport;

    /**
     * @brief RTP receive statistics.
     *
     * Includes received packet count, sorted packet count, last sequence,
     * last timestamp, and other track-level statistics.
     */
    RtpTrackStats _stats;

    /**
     * @brief Callback invoked when a complete media frame is generated.
     */
    FrameCallback _on_frame;
    EncodedFrameCallback _on_encoded_frame;

    SendNackCallback _on_send_nack;
    NackRecoveryFailureCallback _on_nack_recovery_failure;
    media::TrackClock track_clock_;
};


class RtpVideoTracker : public RtpReceiverTrack
{
public:
    using Ptr = std::shared_ptr<RtpVideoTracker>;

    explicit RtpVideoTracker(const TrackInfo& info)
        : RtpReceiverTrack(info)
        , _depacketizer(std::make_unique<H264Depacketizer>(info.fmtp))
        , _nack_receiver(std::make_unique<NackRequester>(NackRequester::Config{}))
    {
        _nack_receiver->SetNackCallback([this](const std::vector<uint16_t> &lost_seq){
            /* notify nack request*/
            if(lost_seq.empty())
            {
                return;
            }

            CountNack(lost_seq.size());

            if (_on_send_nack) 
            {
                _on_send_nack(getSSRC(), lost_seq);
            }
        });
        _nack_receiver->SetRecoveryFailureCallback(
            [this](const std::vector<uint16_t>& abandoned_seq) {
                if (_on_nack_recovery_failure && !abandoned_seq.empty())
                {
                    _on_nack_recovery_failure(getSSRC(), abandoned_seq);
                }
            });
    }

    ~RtpVideoTracker() override = default;

public:
    RtpPacket::Ptr inputRtp(uint8_t* ptr, size_t len) override;

    void TickNack(uint64_t now_ms)
    {
        if (_nack_receiver)
        {
            _nack_receiver->Process(now_ms);
        }
    }

    void UpdateNackRtt(uint32_t rtt_ms)
    {
        if (_nack_receiver)
        {
            _nack_receiver->UpdateRtt(rtt_ms);
        }
    }

    void ClearNackUpTo(uint16_t seq)
    {
        if (_nack_receiver)
        {
            _nack_receiver->ClearUpTo(seq);
        }
    }

    const NackRequester::Stats* GetNackRequesterStats() const
    {
        return _nack_receiver ? &_nack_receiver->GetStats() : nullptr;
    }

    void OnRtcpBye(uint32_t sender_ssrc) override
    {
        CountBye();
    }

protected:
    void onRtpSorted(const RtpPacket::Ptr& pkt) override;

private:


    std::unique_ptr<H264Depacketizer> _depacketizer;
    std::unique_ptr<NackRequester> _nack_receiver;
};

class RtpAudioTracker : public RtpReceiverTrack
{
public:
    using Ptr = std::shared_ptr<RtpAudioTracker>;

    explicit RtpAudioTracker(const TrackInfo& info);
    ~RtpAudioTracker() override;

    /**
     * Create an independent audio receiver track that mirrors future valid
     * RTP packets received by this track.
     *
     * Runtime state (packet sorter, statistics, depacketizer and callbacks)
     * is intentionally not copied. The clone starts empty and remains
     * attached only while its owner keeps the returned shared_ptr alive.
     */
    Ptr Clone();

    RtpPacket::Ptr inputRtp(uint8_t* ptr, size_t len) override;
protected:
    void onRtpSorted(const RtpPacket::Ptr& pkt) override;

private:
    std::vector<Ptr> SnapshotClones();

    std::unique_ptr<media::IAudioRtpDepacketizer> depacketizer_;
    uint64_t audio_packets_seen_ = 0;
    uint64_t audio_frames_completed_ = 0;
    uint64_t audio_depacketize_failures_ = 0;
    std::mutex clones_mutex_;
    std::vector<std::weak_ptr<RtpAudioTracker>> clones_;
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
    using TransportFeedbackCallback = std::function<void(const rtcpx::TransportFeedbackReport&)>;
    using SenderReportCallback = std::function<void(uint32_t media_ssrc)>;
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
    void RemoveReceiverTrack(uint32_t media_ssrc);

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
    void RemoveSenderTrack(uint32_t media_ssrc);
    void SetTransportFeedbackCallback(TransportFeedbackCallback cb);
    void SetSenderReportCallback(SenderReportCallback cb);

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
    void OnTransportFeedback(const rtcpx::TransportFeedbackReport& report) override;
    void OnBye(uint32_t sender_ssrc) override;
    void OnRttUpdated(uint32_t media_ssrc, uint32_t rtt_ms) override;

private:
    std::mutex tracks_mutex_;
    std::unordered_map<uint32_t, std::weak_ptr<RtpReceiverTrack>> recv_tracks_;
    std::unordered_map<uint32_t, std::weak_ptr<RtpSenderTrack>> send_tracks_;
    TransportFeedbackCallback transport_feedback_cb_;
    SenderReportCallback sender_report_cb_;
};
}
#endif // _RTPRECEIVER_H_
