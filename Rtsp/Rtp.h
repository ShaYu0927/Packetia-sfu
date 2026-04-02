#ifndef _RTP_H_
#define _RTP_H_

#include <memory>
#include "RtpTypes.h"
#include <functional>
#include <map>
#include <chrono>
#include <cstring>
#include "logger.h"



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

class Frame;
using FramePtr = std::shared_ptr<Frame>;


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

class RtpRecvStatsBase
{
public:
    RtpRecvStatsBase() = default;
    virtual ~RtpRecvStatsBase() = default;

public:
    void OnFrameCompleted();
    void CountNack(size_t count);
    void CountPli();
    void CountFir();
    void SetRttMs(uint32_t rtt_ms);
    void SetJitter(uint32_t jitter);
    void UpdateSequenceStats(uint16_t seq);

    void Reset()
    {
        _first_rtp_recv_ms = 0;
        _last_rtp_recv_ms = 0;
        _first_frame_ms = 0;
        _last_frame_ms = 0;

        _rtp_packet_count = 0;
        _rtp_bytes = 0;
        _frame_count = 0;

        _duplicate_packets = 0;
        _out_of_order_packets = 0;

        _nack_count = 0;
        _pli_count = 0;
        _fir_count = 0;

        _ssrc = 0;
        _payload_type = 0;
        _last_seq = 0;
        _last_timestamp = 0;

        _rtt_ms = 0;
        _jitter = 0;

        _has_seq = false;
    }

public:
    uint64_t GetFirstRtpRecvMs() const { return _first_rtp_recv_ms; }
    uint64_t GetLastRtpRecvMs() const { return _last_rtp_recv_ms; }
    uint64_t GetFirstFrameMs() const { return _first_frame_ms; }
    uint64_t GetLastFrameMs() const { return _last_frame_ms; }

    uint64_t GetRtpPacketCount() const { return _rtp_packet_count; }
    uint64_t GetRtpBytes() const { return _rtp_bytes; }
    uint64_t GetFrameCount() const { return _frame_count; }

    uint64_t GetDuplicatePackets() const { return _duplicate_packets; }
    uint64_t GetOutOfOrderPackets() const { return _out_of_order_packets; }

    uint64_t GetNackCount() const { return _nack_count; }
    uint64_t GetPliCount() const { return _pli_count; }
    uint64_t GetFirCount() const { return _fir_count; }

    uint32_t GetSsrc() const { return _ssrc; }
    uint8_t GetPayloadType() const { return _payload_type; }
    uint16_t GetLastSeq() const { return _last_seq; }
    uint32_t GetLastTimestamp() const { return _last_timestamp; }

    uint32_t GetRttMs() const { return _rtt_ms; }
    uint32_t GetJitter() const { return _jitter; }

protected:

    static uint64_t NowMs()
    {
        using namespace std::chrono;
        return duration_cast<milliseconds>(
            steady_clock::now().time_since_epoch()).count();
    }


protected:
    uint64_t _first_rtp_recv_ms = 0;
    uint64_t _last_rtp_recv_ms = 0;
    uint64_t _first_frame_ms = 0;
    uint64_t _last_frame_ms = 0;

    uint64_t _rtp_packet_count = 0;
    uint64_t _rtp_bytes = 0;
    uint64_t _frame_count = 0;

    uint64_t _duplicate_packets = 0;
    uint64_t _out_of_order_packets = 0;

    uint64_t _nack_count = 0;
    uint64_t _pli_count = 0;
    uint64_t _fir_count = 0;

    uint32_t _ssrc = 0;
    uint8_t  _payload_type = 0;
    uint16_t _last_seq = 0;
    uint32_t _last_timestamp = 0;

    uint32_t _rtt_ms = 0;
    uint32_t _jitter = 0;

    bool _has_seq = false;
};

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

    struct 
    {
        uint16_t rtp_channel = 0;
        uint16_t rtcp_channel = 0;
    } tcp;

    struct 
    {
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



template<typename Packet, typename Seq = uint16_t>
class EnhancedPacketSortor 
{
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
        if (_cb) 
        {
            _cb(seq, pkt);
        }
        ++_next_seq;
    }

    uint32_t distance(Seq a, Seq b) const 
    {
        return static_cast<uint16_t>(a - b);
    }

private:
    bool _started = false;
    Seq _next_seq = 0;

    std::map<Seq, Packet> _buffer;
    Callback _cb;

    uint16_t _max_gap;
    size_t _max_cache;
    uint32_t _flush_timeout; 

    size_t _lost_count = 0;
    size_t _drop_count = 0;

    std::chrono::steady_clock::time_point _last_flush_time;
};

class RtpPacket 
{
public:
    using Ptr = std::shared_ptr<RtpPacket>;

    static constexpr uint8_t kRtpVersion = 2;
    static constexpr size_t kRtpHeaderSize = 12;
    static constexpr size_t kRtpTcpHeaderSize = 4;
    static constexpr size_t kRtpMaxSize = 1500;

    static Ptr create(size_t capacity = kRtpMaxSize);

public:
    RtpPacket() = default;
    ~RtpPacket() = default;

    uint16_t getSeq() const { return seq_; }
    void setSeq(uint16_t seq) { seq_ = seq; }

    uint32_t getStamp() const { return ts_; }
    void setStamp(uint32_t ts) { ts_ = ts; }

    uint32_t getSSRC() const { return ssrc_; }
    void setSSRC(uint32_t ssrc) { ssrc_ = ssrc; }

    uint8_t getPayloadType() const { return pt_; }
    void setPayloadType(uint8_t pt) { pt_ = pt; }

    bool getMarker() const { return marker_; }
    void setMarker(bool val) { marker_ = val; }

    uint8_t getVersion() const { return version_; }
    void setVersion(uint8_t version) { version_ = version; }

    uint8_t* getData() { return data_.get(); }
    const uint8_t* getData() const { return data_.get(); }

    uint8_t* getPayload() { return data_ ? data_.get() + payload_off_ : nullptr; }
    const uint8_t* getPayload() const { return data_ ? data_.get() + payload_off_ : nullptr; }

    size_t getSize() const { return size_; }
    size_t getCapacity() const { return capacity_; }
    size_t getHeaderLen() const { return hdr_len_; }
    size_t getPayloadSize() const { return payload_len_; }

    void setTrackType(TrackType type) { type_ = type; }
    TrackType getTrackType() const { return type_; }

    void setSampleRate(uint32_t rate) { sample_rate_ = rate; }
    uint32_t getSampleRate() const { return sample_rate_; }

    void setTrackIndex(int index) { track_index_ = index; }
    int getTrackIndex() const { return track_index_; }

    void setRecvTimeMs(uint64_t ms) { recv_time_ms_ = ms; }
    uint64_t getRecvTimeMs() const { return recv_time_ms_; }

    void reset();
    bool assign(const uint8_t* ptr, size_t len,
            uint8_t version,
            bool padding,
            bool extension,
            uint8_t csrcCnt,
            bool marker,
            uint8_t pt,
            uint16_t seq,
            uint32_t timestamp,
            uint32_t ssrc,
            size_t headerLen,
            size_t payloadLen);

private:
    TrackType type_ = TrackInvalid;
    uint32_t sample_rate_ = 90000;
    uint64_t ntp_stamp_ms_ = 0;
    int track_index_ = -1;

    std::shared_ptr<uint8_t[]> data_;
    size_t capacity_ = 0;
    size_t size_ = 0;

    size_t hdr_len_ = kRtpHeaderSize;
    size_t payload_off_ = kRtpHeaderSize;
    size_t payload_len_ = 0;

    bool marker_ = false;
    uint8_t pt_ = 0;
    uint32_t ts_ = 0;
    uint32_t ssrc_ = 0;
    uint16_t seq_ = 0;
    uint8_t version_ = kRtpVersion;

    bool padding_ = false;
    bool extension_ = false;
    uint8_t cc_ = 0;
    uint8_t csrc_count_ = 0;
    std::array<uint32_t, 15> csrc_{};

    uint64_t recv_time_ms_ = 0;
};

// Represents a single media track (per-stream object).
// A track typically corresponds to one audio stream or one video stream.
// It is responsible for handling RTP/RTCP packets of this track,
// maintaining per-track state and statistics, and emitting decoded frames
// or control feedback events (e.g., NACK / PLI / FIR) to upper layers.
//
// Responsibilities:
// 1. Maintain track-level metadata such as track_index, type, codec,
//    SSRC, payload type, and sample rate.
// 2. Process incoming RTP packets belonging to this track and assemble
//    them into media frames when applicable.
// 3. Process incoming RTCP packets and trigger control callbacks
//    (e.g., NACK, PLI, FIR).
// 4. Maintain per-track runtime statistics (packet loss, jitter, etc.).
//
// Notes:
// - RtpTrack models a single media stream.
// - Concrete implementations such as AudioTrack and VideoTrack should
//   derive from this class and implement media-specific logic.

class RtpTrack
{
public:
    using Ptr = std::shared_ptr<RtpTrack>;
    using OnFrameCallback = std::function<void(const FramePtr&)>;
    using OnNackCallback = std::function<void(const uint16_t*, size_t)>;
    using OnPliCallback = std::function<void()>;
    using OnFirCallback = std::function<void(uint8_t)>;

public:
    explicit RtpTrack(const TrackInfo& info)
        :info_(info) {}
    virtual ~RtpTrack() = default;

    virtual TrackType type() const = 0;

public:
    const TrackInfo& getTrackInfo() const { return info_; };
    const RtpTrackStats& getStats() const { return stats_; };

    int getTrackIndex() const { return info_.track_index; };
    TrackType getTrackType() const { return info_.type; };
    CodecId getCodecId() const { return info_.codec_id; };
    uint32_t getSSRC() const { return info_.ssrc; };
    uint8_t getPayloadType() const { return info_.payload_type; };

    void setInterleavedChannel(uint8_t rtp_channel, uint8_t rtcp_channel);
    
public:
    virtual bool onInputRtp(uint8_t* data, size_t len) = 0;
    virtual void onInputRtcp(const uint8_t* data, size_t len) = 0;

public:
    void setOnFrame(OnFrameCallback cb);
    void setOnNack(OnNackCallback cb);
    void setOnPli(OnPliCallback cb);
    void setOnFir(OnFirCallback cb);

private:
    TrackInfo info_;
    RtpTrackStats stats_;


    OnFrameCallback on_frame_;
    OnNackCallback on_nack_;
    OnPliCallback on_pli_;
    OnFirCallback on_fir_;
};

// Manages multiple RTP tracks within a session (track container / dispatcher).
// RtpTracker owns and maintains a collection of RtpTrack instances,
// and is responsible for routing incoming RTP/RTCP packets to the correct track.
//
// Responsibilities:
// 1. Maintain a mapping from track_index to RtpTrack.
// 2. Add / remove / query tracks.
// 3. Dispatch incoming RTP/RTCP packets to the corresponding track.
// 4. Bridge per-track callbacks (e.g., OnFrame) to upper-layer callbacks.
//
// Notes:
// - RtpTracker does not implement media logic itself.
// - It acts as a routing and management layer for multiple tracks.
// - Each RtpTrack handles its own media processing independently.
class RtpTracker
{
public:
    using Ptr = std::shared_ptr<RtpTracker>;
    using TrackPtr = std::shared_ptr<RtpTrack>;
    using OnFrameCallback = std::function<void(int track_index, const FramePtr&)>;
    using OnTrackAddedCallback = std::function<void(int track_index, const TrackPtr&)>;
    using OnTrackRemovedCallback = std::function<void(int track_index)>;

public:
    RtpTracker() = default;
    ~RtpTracker() = default;

public:
    

    /* add tracker */
    bool addTrack(int track_index, const TrackPtr& track);

    /* delete tracker */
    void removeTrack(int track_index);

    void clear();

    /* get tracker */
    TrackPtr getTrack(int track_index) const;

    /* find by type */
    TrackPtr getTrackByType(TrackType type) const;

    bool hasTrack(int track_index) const;

    /* number */
    size_t size() const;

    /* get all tracker */
    std::vector<TrackPtr> getAllTracks() const;

public:
    // RTP 分发
    bool inputRtp(int track_index,
                  TrackType type,
                  int sample_rate,
                  uint8_t* ptr,
                  size_t len);

    // RTCP 分发
    bool inputRtcp(int track_index,
                   const uint8_t* ptr,
                   size_t len);

public:
    void setOnFrame(OnFrameCallback cb);
    void setOnTrackAdded(OnTrackAddedCallback cb);
    void setOnTrackRemoved(OnTrackRemovedCallback cb);

private:
    void bindTrackFrame(int track_index, const TrackPtr& track);

private:
    std::unordered_map<int, TrackPtr> runtime_tracks_;

    OnFrameCallback on_frame_;
    OnTrackAddedCallback on_track_added_;
    OnTrackRemovedCallback on_track_removed_;
};

class AudioTrack : public RtpTrack
{
public:
    explicit AudioTrack(const TrackInfo& info)
        : RtpTrack(info) {}

    TrackType type() const override
    {
        return TrackType::TrackAudio;
    }

protected:
    bool onInputRtp(uint8_t* data, size_t len) override;
    void onInputRtcp(const uint8_t* data, size_t len) override;
};

class VideoTrack : public RtpTrack, public RtpRecvStatsBase
{
public:
    explicit VideoTrack(const TrackInfo& info)
        : RtpTrack(info) {}

    TrackType type() const override
    {
        return TrackType::TrackVideo;
    }

public:
    bool onInputRtp(uint8_t* data, size_t len) override;
    void onInputRtcp(const uint8_t* data, size_t len) override;

private:
    RtpRecvStatsBase _stats;
};


#endif