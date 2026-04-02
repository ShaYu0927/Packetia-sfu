#ifndef _RTPRECEIVER_H_
#define _RTPRECEIVER_H_


#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "RtpTypes.h"
#include "Rtp.h"
#include "RtcpReciver.h"



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
        if (_on_frame) {
            _on_frame(frame);
            return true;
        }
        return false;
    }

    virtual void onBeforeRtpSorted(const RtpPacket::Ptr &pkt);
    virtual void onRtpSorted(const RtpPacket::Ptr &pkt) = 0;

protected:
    TrackInfo _info;
    RtspTransport _rtsp_transport;
    RtpTrackStats _stats;

    FrameCallback _on_frame;
};


class RtpVideoTracker : public RtpReceiverTrack , public rtcpx::IRtcpObserver, public RtpRecvStatsBase
{
public:
    using Ptr = std::shared_ptr<RtpVideoTracker>;

    explicit RtpVideoTracker(const TrackInfo &info)
        : RtpReceiverTrack(info)
    {
    }

    ~RtpVideoTracker() override = default;

public:
    RtpPacket::Ptr inputRtp(TrackType type, int sample_rate, uint8_t *ptr, size_t len) override;

    void inputRtcp(const uint8_t *ptr, size_t len) override;

    void setOnNack(NackCallback cb) { _on_nack = std::move(cb); }
    void setOnPli(PliCallback cb) { _on_pli = std::move(cb); }
    void setOnFir(FirCallback cb) { _on_fir = std::move(cb); }

protected:
    void onBeforeRtpSorted(const RtpPacket::Ptr &pkt) override;

    void onRtpSorted(const RtpPacket::Ptr &pkt) override;

public:
    void OnReceiverReport(uint32_t sender_ssrc, uint32_t media_ssrc, uint8_t fraction_lost, int32_t cumulative_lost,
                          uint32_t highest_seq, uint32_t jitter, uint32_t lsr, uint32_t dlsr) override;

    void OnSenderReport(uint32_t sender_ssrc, uint64_t ntp, uint32_t rtp_ts, uint32_t packet_count, uint32_t octet_count) override;

    void OnNack(uint32_t sender_ssrc, uint32_t media_ssrc, const uint16_t* seqs, size_t count) override;

    void OnPli(uint32_t sender_ssrc, uint32_t media_ssrc) override;

    void OnFir(uint32_t sender_ssrc, uint32_t media_ssrc, uint8_t seq_nr) override;
    
    void OnBye(uint32_t sender_ssrc) override;

    void OnRttUpdated(uint32_t media_ssrc, uint32_t rtt_ms) override;


private:
    NackCallback _on_nack;
    PliCallback _on_pli;
    FirCallback _on_fir;

    uint32_t _rtt_ms = 0;
};

#endif // _RTPRECEIVER_H_