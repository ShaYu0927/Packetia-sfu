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

/**
 * Per-SSRC receive-side RTP/RTCP statistics.
 *
 * RTP packets update sequence, loss, jitter and receive-rate state as they
 * arrive. Sender Reports update sender-rate and clock state. The periodic
 * RTCP task calls BuildReceiverReport() to obtain one RFC 3550 report block.
 * All methods are expected to run on the media worker that owns the stream.
 */
class RtpRecvStatsBase
{
public:
    /** Values serialized into one RTCP Receiver Report block. */
    struct ReceiverReport
    {
        uint32_t media_ssrc = 0;
        uint8_t fraction_lost = 0;
        int32_t cumulative_lost = 0;
        uint32_t extended_highest_seq = 0;
        uint32_t jitter = 0;
        uint32_t lsr = 0;
        uint32_t dlsr = 0;
    };

    RtpRecvStatsBase() = default;
    virtual ~RtpRecvStatsBase() = default;

public:
    /** Record that one complete encoded media frame was produced. */
    void OnFrameCompleted();

    /** Record a generated NACK and the number of lost RTP packets requested. */
    void CountNack(size_t count);

    /** Record one Picture Loss Indication event. */
    void CountPli();

    /** Record one Full Intra Request event. */
    void CountFir();

    /** Record one RTCP BYE event. */
    void CountBye();

    /**
     * Update receive statistics for one RTP packet.
     *
     * This method updates sequence-number rollover, duplicate/out-of-order
     * counts, RFC 3550 interarrival jitter and the average receive bitrate.
     * It must be called once for every accepted RTP packet, before packet
     * sorting can discard duplicates or late packets.
     *
     * @param ssrc RTP synchronization source identifier.
     * @param payload_type RTP payload type.
     * @param seq RTP 16-bit sequence number.
     * @param rtp_ts RTP timestamp in clock_rate units.
     * @param bytes RTP payload bytes carried by this packet.
     * @param receive_ms Local monotonic receive time in milliseconds.
     * @param clock_rate RTP clock rate in Hz.
     */
    void OnRtpPacket(uint32_t ssrc, uint8_t payload_type, uint16_t seq,
                     uint32_t rtp_ts, size_t bytes, uint64_t receive_ms,
                     uint32_t clock_rate);

    /**
     * Update statistics from one received RTCP Sender Report.
     *
     * Consecutive reports are used to calculate SR interval, sender payload
     * bitrate, packet rate, measured RTP clock rate and clock drift.
     *
     * @param sender_ssrc SSRC of the remote RTP sender.
     * @param ntp 64-bit NTP timestamp from the SR.
     * @param rtp_ts RTP timestamp corresponding to ntp.
     * @param packet_count Sender's cumulative RTP packet count.
     * @param octet_count Sender's cumulative RTP payload octet count.
     * @param receive_ms Local monotonic SR arrival time in milliseconds.
     * @param clock_rate Negotiated RTP clock rate in Hz.
     */
    void OnSenderReport(uint32_t sender_ssrc, uint64_t ntp, uint32_t rtp_ts,
                        uint32_t packet_count, uint32_t octet_count,
                        uint64_t receive_ms, uint32_t clock_rate);

    /**
     * Build the current RTCP Receiver Report block.
     *
     * Calculates interval fraction lost, cumulative loss, extended highest
     * sequence, jitter, LSR and DLSR. Calling this method advances the
     * expected/received interval baseline, so it should be called exactly
     * once for each RR reporting period.
     *
     * @param now_ms Local monotonic time at which the RR will be generated.
     */
    ReceiverReport BuildReceiverReport(uint64_t now_ms);

    /** Store the latest report block received for a locally sent RTP stream. */
    void OnReceiverReport(uint32_t sender_ssrc, uint32_t media_ssrc, uint8_t fraction_lost, int32_t cumulative_lost, uint32_t highest_seq, uint32_t jitter, uint32_t lsr, uint32_t dlsr);

    /** Replace the latest round-trip time estimate, in milliseconds. */
    void SetRttMs(uint32_t rtt_ms);

    /** Replace the latest interarrival jitter value, in RTP clock units. */
    void SetJitter(uint32_t jitter);

    /** Clear all RTP, RTCP, interval and derived metric state. */
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
        _nack_packet_count = 0;
        _pli_count = 0;
        _fir_count = 0;
        _bye_count = 0;
        _sr_count = 0;
        _rr_count = 0;

        _ssrc = 0;
        _payload_type = 0;
        _last_seq = 0;
        _last_timestamp = 0;

        _rtt_ms = 0;
        _jitter = 0;
        _rtcp_sender_ssrc = 0;
        _rtcp_media_ssrc = 0;
        _last_sr_ntp = 0;
        _last_sr_rtp_ts = 0;
        _last_sr_packet_count = 0;
        _last_sr_octet_count = 0;
        _last_sr_receive_ms = 0;
        _sr_interval_ms = 0.0;
        _sender_bitrate_bps = 0.0;
        _sender_packet_rate = 0.0;
        _receiver_bitrate_bps = 0.0;
        _measured_clock_rate = 0.0;
        _clock_drift_ppm = 0.0;
        _last_rr_fraction_lost = 0;
        _last_rr_cumulative_lost = 0;
        _last_rr_highest_seq = 0;
        _last_rr_lsr = 0;
        _last_rr_dlsr = 0;

        _has_seq = false;
        _base_seq = 0;
        _max_seq = 0;
        _seq_cycles = 0;
        _expected_prior = 0;
        _received_prior = 0;
        _jitter_value = 0.0;
        _previous_transit = 0;
        _has_transit = false;
    }

public:
    // RTP/frame timing and cumulative counters.
    uint64_t GetFirstRtpRecvMs() const { return _first_rtp_recv_ms; }
    uint64_t GetLastRtpRecvMs() const { return _last_rtp_recv_ms; }
    uint64_t GetFirstFrameMs() const { return _first_frame_ms; }
    uint64_t GetLastFrameMs() const { return _last_frame_ms; }

    uint64_t GetRtpPacketCount() const { return _rtp_packet_count; }
    uint64_t GetRtpBytes() const { return _rtp_bytes; }
    uint64_t GetFrameCount() const { return _frame_count; }

    uint64_t GetDuplicatePackets() const { return _duplicate_packets; }
    uint64_t GetOutOfOrderPackets() const { return _out_of_order_packets; }

    // RTCP feedback and report counters.
    uint64_t GetNackCount() const { return _nack_count; }
    uint64_t GetNackPacketCount() const { return _nack_packet_count; }
    uint64_t GetPliCount() const { return _pli_count; }
    uint64_t GetFirCount() const { return _fir_count; }
    uint64_t GetByeCount() const { return _bye_count; }
    uint64_t GetSenderReportCount() const { return _sr_count; }
    uint64_t GetReceiverReportCount() const { return _rr_count; }

    // Identity and most recently observed RTP header values.
    uint32_t GetSsrc() const { return _ssrc; }
    uint8_t GetPayloadType() const { return _payload_type; }
    uint16_t GetLastSeq() const { return _last_seq; }
    uint32_t GetLastTimestamp() const { return _last_timestamp; }

    // Latest RTCP values and derived receive-quality metrics.
    uint32_t GetRttMs() const { return _rtt_ms; }
    uint32_t GetJitter() const { return _jitter; }
    uint32_t GetRtcpSenderSsrc() const { return _rtcp_sender_ssrc; }
    uint32_t GetRtcpMediaSsrc() const { return _rtcp_media_ssrc; }
    uint64_t GetLastSrNtp() const { return _last_sr_ntp; }
    uint32_t GetLastSrRtpTimestamp() const { return _last_sr_rtp_ts; }
    uint32_t GetLastSrPacketCount() const { return _last_sr_packet_count; }
    uint32_t GetLastSrOctetCount() const { return _last_sr_octet_count; }
    uint64_t GetLastSrReceiveMs() const { return _last_sr_receive_ms; }
    double GetSrIntervalMs() const { return _sr_interval_ms; }
    double GetSenderBitrateBps() const { return _sender_bitrate_bps; }
    double GetSenderPacketRate() const { return _sender_packet_rate; }
    double GetReceiverBitrateBps() const { return _receiver_bitrate_bps; }
    double GetMeasuredClockRate() const { return _measured_clock_rate; }
    double GetClockDriftPpm() const { return _clock_drift_ppm; }
    uint8_t GetLastRrFractionLost() const { return _last_rr_fraction_lost; }
    int32_t GetLastRrCumulativeLost() const { return _last_rr_cumulative_lost; }
    uint32_t GetLastRrHighestSeq() const { return _last_rr_highest_seq; }

protected:
    /** Return local monotonic time in milliseconds for interval accounting. */
    static uint64_t NowMs()
    {
        using namespace std::chrono;
        return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    }


protected:
    uint64_t _first_rtp_recv_ms = 0;          // First RTP packet receive time in ms
    uint64_t _last_rtp_recv_ms = 0;           // Last RTP packet receive time in ms
    uint64_t _first_frame_ms = 0;             // First frame time in ms
    uint64_t _last_frame_ms = 0;              // Last frame time in ms

    uint64_t _rtp_packet_count = 0;           // Total number of received RTP packets
    uint64_t _rtp_bytes = 0;                  // Total number of received RTP bytes
    uint64_t _frame_count = 0;                // Total number of received frames

    uint64_t _duplicate_packets = 0;          // Number of duplicate RTP packets
    uint64_t _out_of_order_packets = 0;       // Number of out-of-order RTP packets

    uint64_t _nack_count = 0;                 // Number of received NACK messages
    uint64_t _nack_packet_count = 0;          // Number of packets requested by NACK
    uint64_t _pli_count = 0;                  // Number of received PLI messages
    uint64_t _fir_count = 0;                  // Number of received FIR messages
    uint64_t _bye_count = 0;                  // Number of received RTCP BYE messages
    uint64_t _sr_count = 0;                   // Number of received Sender Reports
    uint64_t _rr_count = 0;                   // Number of received Receiver Reports

    uint32_t _ssrc = 0;                       // RTP stream SSRC
    uint8_t  _payload_type = 0;               // RTP payload type
    uint16_t _last_seq = 0;                   // Last received RTP sequence number
    uint32_t _last_timestamp = 0;             // Last received RTP timestamp

    uint32_t _rtt_ms = 0;                     // Latest round-trip time in ms
    uint32_t _jitter = 0;                     // Latest RTP interarrival jitter
    uint32_t _rtcp_sender_ssrc = 0;            // SSRC of the RTCP sender
    uint32_t _rtcp_media_ssrc = 0;             // Media SSRC referenced by RTCP
    uint64_t _last_sr_ntp = 0;                 // NTP timestamp from the latest SR
    uint32_t _last_sr_rtp_ts = 0;              // RTP timestamp from the latest SR
    uint32_t _last_sr_packet_count = 0;        // Sender packet count from the latest SR
    uint32_t _last_sr_octet_count = 0;         // Sender octet count from the latest SR
    uint64_t _last_sr_receive_ms = 0;          // Local arrival time of the latest SR
    double _sr_interval_ms = 0.0;
    double _sender_bitrate_bps = 0.0;
    double _sender_packet_rate = 0.0;
    double _receiver_bitrate_bps = 0.0;
    double _measured_clock_rate = 0.0;
    double _clock_drift_ppm = 0.0;
    uint8_t _last_rr_fraction_lost = 0;        // Fraction lost from the latest RR
    int32_t _last_rr_cumulative_lost = 0;      // Cumulative packet loss from the latest RR
    uint32_t _last_rr_highest_seq = 0;         // Extended highest sequence from the latest RR
    uint32_t _last_rr_lsr = 0;                 // Last Sender Report timestamp from RR
    uint32_t _last_rr_dlsr = 0;                // Delay since last Sender Report from RR

    bool _has_seq = false;                     // Whether the RTP sequence number is initialized
    uint16_t _base_seq = 0;
    uint16_t _max_seq = 0;
    uint32_t _seq_cycles = 0;
    uint32_t _expected_prior = 0;
    uint32_t _received_prior = 0;
    double _jitter_value = 0.0;
    int32_t _previous_transit = 0;
    bool _has_transit = false;
};

class RtpHeader
{
public:
    static constexpr size_t kSize = 12;

    bool InputFromBuffer(const uint8_t* buf, size_t len);
    void serialize(uint8_t* out) const;

    uint8_t getVersion() const { return _version; }
    void setVersion(uint8_t ver) { _version = ver; }

    bool getPadding() const { return _padding; }
    bool getExtension() const { return _extension; }
    uint8_t getCsrcCount() const { return _csrc; }

    uint8_t getPayloadType() const { return _payload_type; }
    void setPayloadType(uint8_t pt) { _payload_type = pt; }

    bool getMarker() const { return _marker; }
    void setMarker(bool marker) { _marker = marker; }

    uint16_t getSequence() const { return _seq; }
    void setSequence(uint16_t seq) { _seq = seq; }

    uint32_t getTimestamp() const { return _timestamp; }
    void setTimestamp(uint32_t ts) { _timestamp = ts; }

    uint32_t getSSRC() const { return _ssrc; }
    void setSSRC(uint32_t ssrc) { _ssrc = ssrc; }

    size_t getHeaderSize() const { return kSize + static_cast<size_t>(_csrc) * 4; }

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
    using Callback = std::function<void(Seq seq, Packet pkt)>;

    EnhancedPacketSortor(uint16_t max_gap = 1000,
                         size_t max_cache = 50,
                         uint32_t flush_timeout_ms = 100)
        : _max_gap(max_gap),
          _max_cache(max_cache),
          _flush_timeout(flush_timeout_ms) {}

    void setOnPacketSorted(Callback cb)
    {
        _cb = std::move(cb);
    }

    void inputPacket(Seq seq, Packet pkt)
    {
        auto now = std::chrono::steady_clock::now();
        if (!_started)
        {
            _next_seq = seq;
            _last_flush_time = now;
            _started = true;
            emit(seq, std::move(pkt));
            return;
        }

        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - _last_flush_time).count() > _flush_timeout)
        {
            flushTimeout();
            _last_flush_time = now;
        }

        if (seq == _next_seq)
        {
            emit(seq, std::move(pkt));
            flushBuffered();
            return;
        }

        if (isOlder(seq, _next_seq))
        {
            ++_drop_count;
            return;
        }

        if (distance(seq, _next_seq) < _max_gap)
        {
            auto [it, inserted] = _buffer.emplace(seq, std::move(pkt));
            if (!inserted)
            {
                ++_drop_count;
            }

            while (_buffer.size() > _max_cache)
            {
                ++_lost_count;
                ++_next_seq;
                flushBuffered();
            }
            return;
        }

        ++_drop_count;
    }

    void flushBuffered()
    {
        while (true)
        {
            auto it = _buffer.find(_next_seq);
            if (it == _buffer.end())
            {
                break;
            }

            auto pkt = std::move(it->second);
            _buffer.erase(it);
            emit(_next_seq, std::move(pkt));
        }
    }

    void flushTimeout()
    {
        while (!_buffer.empty())
        {
            auto it = _buffer.find(_next_seq);
            if (it != _buffer.end())
            {
                auto pkt = std::move(it->second);
                _buffer.erase(it);
                emit(_next_seq, std::move(pkt));
                continue;
            }

            auto first = _buffer.begin();
            if (distance(first->first, _next_seq) < _max_gap)
            {
                ++_lost_count;
                ++_next_seq;
                continue;
            }

            break;
        }
    }

    size_t getLostCount() const { return _lost_count; }
    size_t getDropCount() const { return _drop_count; }
    size_t getBufferedCount() const { return _buffer.size(); }

    void flushExpired()
    {
        if (!_started)
        {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                now - _last_flush_time).count() > _flush_timeout)
        {
            flushTimeout();
            _last_flush_time = now;
        }
    }

private:
    void emit(Seq seq, Packet pkt)
    {
        if (_cb)
        {
            _cb(seq, std::move(pkt));
        }
        ++_next_seq;
    }

    uint32_t distance(Seq a, Seq b) const
    {
        return static_cast<uint16_t>(a - b);
    }

    bool isOlder(Seq a, Seq b) const
    {
        return a != b && distance(a, b) >= 0x8000;
    }

private:
    bool _started = false;
    Seq _next_seq = 0;

    std::map<Seq, Packet> _buffer;
    Callback _cb;

    uint16_t _max_gap = 1000;
    size_t _max_cache = 50;
    uint32_t _flush_timeout = 100;

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

    uint8_t* getPayload() 
    {
        uint8_t *payload = data_ ? data_.get() + payload_off_ : nullptr;
        return payload;
    }
    const uint8_t* getPayload() const 
    { 
        const uint8_t *payload = data_ ? data_.get() + payload_off_ : nullptr;
        return payload; 
    }

    size_t getSize() const { return size_; }
    size_t getCapacity() const { return capacity_; }
    size_t getHeaderLen() const { return hdr_len_; }
    size_t getPayloadSize() const { return payload_len_; }
    void setHeaderInfo(size_t header_len, size_t payload_offset, size_t payload_len)
    {
        hdr_len_ = header_len;
        payload_off_ = payload_offset;
        payload_len_ = payload_len;
    }

    void setTrackType(TrackType type) { type_ = type; }
    TrackType getTrackType() const { return type_; }

    void setSampleRate(uint32_t rate) { sample_rate_ = rate; }
    uint32_t getSampleRate() const { return sample_rate_; }

    void setTrackIndex(int index) { track_index_ = index; }
    int getTrackIndex() const { return track_index_; }

    void setRecvTimeMs(uint64_t ms) { recv_time_ms_ = ms; }
    uint64_t getRecvTimeMs() const { return recv_time_ms_; }

    void SetSequence(uint16_t seq) { seq_ = seq; }

    void setPayload(const uint8_t* payload, size_t len);

    void setRaw(const uint8_t* data, size_t len);

    bool reserve(size_t capacity);

    void reset();
    void resetForReuse();

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
    using PacketPtr = RtpPacket::Ptr;
    explicit VideoTrack(const TrackInfo& info)
        : RtpTrack(info) 
        {}

    TrackType type() const override
    {
        return TrackType::TrackVideo;
    }

public:
    bool onInputRtp(uint8_t* data, size_t len) override;
    void onInputRtcp(const uint8_t* data, size_t len) override;

private:
    void onOrderedPacket(uint16_t seq, PacketPtr pkt);

private:
    RtpRecvStatsBase _stats;
    EnhancedPacketSortor<PacketPtr, uint16_t> sorter_;
};


#endif
