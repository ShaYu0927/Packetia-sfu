#ifndef _RTP_H_
#define _RTP_H_

#include <memory>
#include <string>
#include <vector>
#include "RtpTypes.h"
#include <functional>
#include <map>
#include <chrono>
#include <cstring>


#include "logger.h"
#include "H264Depacketizer.h"
#include "RtcpReciver.h"
#include "Rtsp.h"


/*
    https://www.rfc-editor.org/rfc/rfc3550.pdf
*/



class Sdp;

#pragma pack(push,1)
struct RtpWireHeader 
{
    uint8_t vpxcc;
    uint8_t mpt;
    uint16_t seq;
    uint32_t ts;
    uint32_t ssrc;
};
#pragma pack(pop)

static_assert(sizeof(RtpWireHeader) == 12);


/*

      0                   1                   2                   3
    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    |V=2|P|X|  CC   |M|     PT      |       sequence number         |
    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    |                           timestamp                           |
    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    |           synchronization source (SSRC) identifier            |
    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
*/

class RtpHeader 
{

public:
    static constexpr size_t kSize = 12;

    RtpHeader() {}
    ~RtpHeader() {}

    void serialize(uint8_t* out) const;

    uint8_t getVersion() const;
    void setVersion(uint8_t ver);

    uint8_t getPayloadType() const;
    void setPayloadType(uint8_t pt);

    bool getMarker() const;
    void setMarker(bool marker);

    uint16_t getSequence() const;
    void setSequence(uint16_t seq);

    uint32_t getTimestamp() const;
    void setTimestamp(uint32_t ts);

    uint32_t getSSRC() const;
    void setSSRC(uint32_t ssrc);

	  
private:
    uint8_t _version = 2;
    bool _padding = false;
    bool _extension = false;
    uint8_t _csrc = 0;

    bool _marker = false;
    uint8_t _payload_type = 0;

    uint16_t _seq = 0;
    uint32_t _timestamp = 0;
    uint32_t _ssrc = 0;
    uint16_t pt;
};

struct RtpTransportTcp 
{
    uint16_t rtp_channel = 0;
    uint16_t rtcp_channel = 0;
};

struct RtpTransportUdp 
{
    uint16_t rtp_port = 0;
    uint16_t rtcp_port = 0;
};

struct RtpTransportInfo 
{
    MediaTransportType transport_type = MediaTransportType::UNKNOWN;

    struct {
        uint16_t rtp_channel = 0;
        uint16_t rtcp_channel = 0;
    } tcp;

    struct {
        uint16_t rtp_port = 0;
        uint16_t rtcp_port = 0;
    } udp;
};

struct RtcpStats 
{
    uint64_t packet_count = 0;
    uint64_t octet_count = 0;
    uint64_t last_ntp_time = 0;
};

enum class MediaSessionState : uint8_t 
{
    Init   = 0,
    Setup  = 1,
    Play   = 2,
    Record = 4,
};

inline MediaSessionState operator|(MediaSessionState a, MediaSessionState b) 
{
    return static_cast<MediaSessionState>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}
inline bool hasState(MediaSessionState state, MediaSessionState flag) 
{
    return (static_cast<uint8_t>(state) & static_cast<uint8_t>(flag)) != 0;
}

struct MediaChannelInfo 
{
    RtpHeader rtp_header;

    RtpTransportInfo transport;

    uint16_t rtp_sequence = 0;
    uint32_t clock_rate = 90000;  // default for video

    RtcpStats rtcp_stats;

    MediaSessionState state = MediaSessionState::Init;

    void markSetup()  { state = state | MediaSessionState::Setup; }
    void markPlay()   { state = state | MediaSessionState::Play; }
    void markRecord() { state = state | MediaSessionState::Record; }

    bool isSetup() const  { return hasState(state, MediaSessionState::Setup); }
    bool isPlay() const   { return hasState(state, MediaSessionState::Play); }
    bool isRecord() const { return hasState(state, MediaSessionState::Record); }
};

template<typename Packet, typename Seq = uint16_t>
class EnhancedPacketSortor {
public:
    using Callback = std::function<void(Seq seq, const Packet& pkt)>;

    EnhancedPacketSortor(uint16_t max_gap = 1000, size_t max_cache = 50, uint32_t flush_timeout_ms = 100)
        : _max_gap(max_gap), _max_cache(max_cache), _flush_timeout(flush_timeout_ms) {}

    void setOnPacketSorted(Callback cb) 
    {
        _cb = std::move(cb);
    }

    void inputPacket(Seq seq, Packet pkt) 
    {
        auto now = std::chrono::steady_clock::now();
#if RTP_DEBUG
        LOG_INFO("[Sort] in seq=", seq,
        " next=", _next_seq,
        " obj=", (void*)(pkt ? pkt.get() : nullptr),
        " use_count=", (pkt ? pkt.use_count() : 0),
        " pkt.seq_=", (pkt ? pkt->seq_ : 0),
        " ts=", (pkt ? pkt->ts : 0),
        " payload_len=", (pkt ? pkt->size : 0));
#endif


        if (!_started) 
        {
            _next_seq = seq;
            _last_flush_time = now;
            _started = true;
            emit(seq, pkt);
            return;
        }

        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - _last_flush_time).count() > _flush_timeout) 
        {
            flushBuffered();
            _last_flush_time = now;
        }

        if (seq == _next_seq) 
        {
            emit(seq, pkt);
            flushBuffered();
        } 
        else if (distance(seq, _next_seq) < _max_gap) 
        {
            _buffer[seq] = std::move(pkt);
            if (_buffer.size() > _max_cache) 
            {
                ++_lost_count;
                LOG_INFO("[PacketSortor] too much cache, force drop seq=", _next_seq);
                ++_next_seq;
                flushBuffered();
            }
        } 
        else 
        {
            ++_drop_count;
        }
    }

    void flushBuffered() 
    {
        while (!_buffer.empty()) 
        {
            auto it = _buffer.find(_next_seq);
            if (it == _buffer.end())
                break;

            emit(it->first, it->second);
            _buffer.erase(it);
        }
    }

    size_t getLostCount() const { return _lost_count; }
    size_t getDropCount() const { return _drop_count; }

private:
    void emit(Seq seq, const Packet& pkt) 
    {
#if RTP_DEBUG
        LOG_INFO("[Emitter] emit begin: seq=", seq,
            " next_seq=", _next_seq,
            " cb=", (void*)(_cb ? (void*)1 : nullptr),
            " sp_addr=", (void*)&pkt,
            " obj=", (void*)(pkt ? pkt.get() : nullptr),
            " use_count=", (pkt ? pkt.use_count() : 0));
#endif
        if (_cb) 
        {
            _cb(seq, pkt);
        }
        ++_next_seq;
    }

    uint32_t distance(Seq a, Seq b) const 
    {
        return static_cast<uint16_t>(a - b); // 支持回绕
    }

private:
    bool _started = false;
    Seq _next_seq = 0;

    std::map<Seq, Packet> _buffer;
    Callback _cb;

    uint16_t _max_gap;
    size_t _max_cache;
    uint32_t _flush_timeout; // milliseconds

    size_t _lost_count = 0;
    size_t _drop_count = 0;

    std::chrono::steady_clock::time_point _last_flush_time;
};

class RtpPacket : public std::enable_shared_from_this<RtpPacket>
{
public:
    using Ptr = std::shared_ptr<RtpPacket>;
    RtpPacket();
    ~RtpPacket();

    static constexpr int kRtpVersion = 2;
    static constexpr int kRtpHeaderSize = 12;
    static constexpr int kRtpTcpHeaderSize = 4;
    static constexpr int kRtpMaxSize = 1500;  


    static std::shared_ptr<RtpPacket> create(size_t capacity = kRtpMaxSize);


    uint16_t getSeq() const;
    void setSeq(uint16_t seq);

    uint32_t getStamp() const;
    void setStamp(uint32_t ts);

    uint64_t getStampMS(bool ntp = true) const;

    uint32_t getSSRC() const;
    void setSSRC(uint32_t ssrc);

    uint8_t* getPayload();                 
    size_t getPayloadSize() const;        

    void setPayload(const uint8_t* payload_data, size_t len);

    void setMarker(bool val);
    bool getMarker() const;

    void setPayloadType(uint8_t pt);
    uint8_t getPayloadType() const;

    void setVersion(uint8_t version);
    uint8_t getVersion() const;
    
public:
    TrackType type          = TrackInvalid;
    uint32_t sample_rate    = 90000;
    uint64_t ntp_stamp_ms   = 0;
    int track_index         = -1;
    std::shared_ptr<uint8_t[]> data;
    size_t capacity = 0;  
    size_t size = 0;
    uint16_t hdr_len;
    uint16_t payload_off;
    uint32_t payload_len;
    bool marker;
    uint8_t pt;
    uint32_t ts;
    uint32_t ssrc;
    uint32_t seq_;
    uint8_t version;
    bool padding;
    bool extension;
    uint8_t cc;
    uint8_t csrc_count;
    std::array<uint32_t, 15> csrc;
    uint32_t recv_time_ms;
};



class RtpTrack : public EnhancedPacketSortor<RtpPacket::Ptr, uint16_t> 
{
public:
    using Ptr = std::shared_ptr<RtpTrack>;

    RtpTrack(TrackType type, std::string codec, uint8_t payload_type,
             uint32_t ssrc, uint32_t clock_rate, uint8_t channel_id = 0, bool disable_ntp = false)
       : _type(type),
        _codec(std::move(codec)),
        _ssrc(ssrc),
        _sample_rate(clock_rate),
        _channel_id(channel_id),
        _disable_ntp(disable_ntp),
        _pt(payload_type) 
        {
            setOnPacketSorted(
                [this](uint16_t seq, const RtpPacket::Ptr& pkt) {
                    this->onRtpSorted(pkt);
                }
            );
        }


    uint32_t getSSRC() const { return _ssrc; }
    uint8_t  getPayloadType() const { return _pt; }
    TrackType getType() const { return _type; }
    uint32_t getSampleRate() const { return _sample_rate; }

    virtual RtpPacket::Ptr inputRtp(TrackType type, int sample_rate, uint8_t *ptr, size_t len) = 0;
    virtual void inputRtcp(const uint8_t* ptr, size_t len) = 0;

    void setNtpStamp(uint32_t rtp_stamp, uint64_t ntp_stamp_ms);
    void setPayloadType(uint8_t pt) { _pt = pt; }
    
    void setRtspTransportInfo(const RtspTransport& info) { _rtsp_transport = info; }
    const RtspTransport& getRtspTransportInfo() const { return _rtsp_transport; }

    void setInterleavedChannel(int rtp_ch, int rtcp_ch)
    {
        _rtsp_transport.transport = RtspTransportType::TcpInterleaved;
        _rtsp_transport.interleaved_rtp = rtp_ch;
        _rtsp_transport.interleaved_rtcp = rtcp_ch;
    }

    int getRtpChannel() const { return _rtsp_transport.interleaved_rtp; }
    int getRtcpChannel() const { return _rtsp_transport.interleaved_rtcp; }
    bool isTcpInterleaved() const
    {
        return _rtsp_transport.transport == RtspTransportType::TcpInterleaved;
    }

    void setMode(const RtspMode& mode) { _rtsp_transport.mode = mode; }
    const RtspMode& getMode() const { return _rtsp_transport.mode; }


   
protected:
    virtual void onRtpSorted(RtpPacket::Ptr rtp) {}
    virtual void onBeforeRtpSorted(const RtpPacket::Ptr &rtp) {}

private:
    TrackType _type;
    std::string _codec;
    uint32_t _ssrc = 0;
    uint32_t _sample_rate = 0;
    uint8_t _channel_id = 0;
    bool _disable_ntp = false;
    uint8_t _pt = 0xFF;
    RtspTransport _rtsp_transport;
};



class RtpVideoTracker : public RtpTrack, public rtcpx::IRtcpObserver
{
public:
    using Ptr = std::shared_ptr<RtpVideoTracker>;
     RtpVideoTracker(TrackType type,
                    const std::string& codec,
                    uint8_t payload_type,
                    uint32_t ssrc,
                    uint32_t clock_rate,
                    uint8_t channel_id = 0,
                    bool disable_ntp = false)
        : RtpTrack(type, codec, payload_type, ssrc, clock_rate, channel_id, disable_ntp)
    {
        if (codec == "H264" || codec == "h264") 
        {
            depacketizer_ = std::make_unique<H264Depacketizer>();
            rtcp_packet_ = std::make_unique<rtcpx::RtcpReceiverImpl>(this);
        } 
        else 
        {
            depacketizer_.reset(); 
        }
    }
    RtpPacket::Ptr inputRtp(TrackType type, int sample_rate, uint8_t *ptr, size_t len) override;


protected:
    void onRtpSorted(RtpPacket::Ptr rtp) override;
    void inputRtcp(const uint8_t* ptr, size_t len) override;
    void OnReceiverReport(uint32_t sender_ssrc, const std::vector<rtcpx::RrBlock>& blocks) override;
    void OnPli(uint32_t media_ssrc) override;
    void OnNack(uint32_t media_ssrc, const std::vector<uint16_t>& missing_seq) override;

private:
    std::vector<RtpPacket::Ptr> cache_;
    std::vector<uint8_t> nalu_buf_;
    uint32_t cur_ts_ = 0;
    bool has_ts_ = false;
    bool assembling_fu_ = false;
    uint8_t fu_nal_type_ = 0;

    static inline void append_start_code(std::vector<uint8_t>& out);
    
    bool isKeyFrame(const RtpPacket::Ptr& pkt);
    std::unique_ptr<H264Depacketizer> depacketizer_;
    std::unique_ptr<rtcpx::RtcpReceiverImpl> rtcp_packet_;
};

class RtpAudioTracker : public RtpTrack
{
public:
    RtpAudioTracker(TrackType type,
                    const std::string& codec,
                    uint8_t payload_type,
                    uint32_t ssrc,
                    uint32_t clock_rate,
                    uint8_t channel_id = 0,
                    bool disable_ntp = false)
        : RtpTrack(type, codec, payload_type, ssrc, clock_rate, channel_id, disable_ntp)
    {}
    RtpPacket::Ptr inputRtp(TrackType type, int sample_rate, uint8_t *ptr, size_t len) override;

    virtual void inputRtcp(const uint8_t* ptr, size_t len) override { (void)ptr; (void)len; }
};
#endif