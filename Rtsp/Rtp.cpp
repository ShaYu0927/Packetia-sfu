#include "Rtp.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>


static inline std::string hex8(uint8_t v)
{
    const char* H = "0123456789ABCDEF";
    std::string s = "00";
    s[0] = H[v >> 4];
    s[1] = H[v & 0x0F];
    return s;
}

void RtpRecvStatsBase::OnFrameCompleted()
{
    const uint64_t now_ms = NowMs();
    if (_first_frame_ms == 0)
    {
        _first_frame_ms = now_ms;
    }
    _last_frame_ms = now_ms;
    ++_frame_count;
}

void RtpRecvStatsBase::CountNack(size_t count)
{
    ++_nack_packet_count;
    _nack_count += count;
}

void RtpRecvStatsBase::CountPli()
{
    ++_pli_count;
}

void RtpRecvStatsBase::CountFir()
{
    ++_fir_count;
}

void RtpRecvStatsBase::CountBye()
{
    ++_bye_count;
}

void RtpRecvStatsBase::OnRtpPacket(uint32_t ssrc, uint8_t payload_type,
                                   uint16_t seq, uint32_t rtp_ts,
                                   size_t bytes, uint64_t receive_ms,
                                   uint32_t clock_rate)
{
    if (_first_rtp_recv_ms == 0)
    {
        _first_rtp_recv_ms = receive_ms;
    }
    _last_rtp_recv_ms = receive_ms;
    _ssrc = ssrc;
    _payload_type = payload_type;
    _last_seq = seq;
    _last_timestamp = rtp_ts;
    ++_rtp_packet_count;
    _rtp_bytes += bytes;

    if (!_has_seq)
    {
        _has_seq = true;
        _base_seq = seq;
        _max_seq = seq;
    }
    else
    {
        const int16_t delta = static_cast<int16_t>(seq - _max_seq);
        if (delta > 0)
        {
            if (seq < _max_seq)
            {
                _seq_cycles += 1U << 16;
            }
            _max_seq = seq;
        }
        else if (delta == 0)
        {
            ++_duplicate_packets;
        }
        else
        {
            ++_out_of_order_packets;
        }
    }

    if (clock_rate > 0)
    {
        const uint32_t arrival_rtp_units = static_cast<uint32_t>(
            (receive_ms * static_cast<uint64_t>(clock_rate)) / 1000ULL);
        const int32_t transit = static_cast<int32_t>(arrival_rtp_units - rtp_ts);
        if (_has_transit)
        {
            const int64_t difference = std::llabs(
                static_cast<int64_t>(transit) - _previous_transit);
            _jitter_value += (static_cast<double>(difference) - _jitter_value) / 16.0;
            _jitter = static_cast<uint32_t>(std::max(0.0, _jitter_value));
        }
        _previous_transit = transit;
        _has_transit = true;
    }

    if (_first_rtp_recv_ms != 0 && receive_ms > _first_rtp_recv_ms)
    {
        const uint64_t elapsed_ms = receive_ms - _first_rtp_recv_ms;
        _receiver_bitrate_bps = static_cast<double>(_rtp_bytes) * 8000.0 /
                                static_cast<double>(elapsed_ms);
    }
}

void RtpRecvStatsBase::OnSenderReport(uint32_t sender_ssrc, uint64_t ntp,
                                      uint32_t rtp_ts, uint32_t packet_count,
                                      uint32_t octet_count, uint64_t receive_ms,
                                      uint32_t clock_rate)
{
    if (_sr_count > 0 && ntp > _last_sr_ntp)
    {
        const double interval_seconds =
            static_cast<double>(ntp - _last_sr_ntp) / 4294967296.0;
        if (interval_seconds > 0.0)
        {
            _sr_interval_ms = interval_seconds * 1000.0;
            const uint32_t octet_delta = octet_count - _last_sr_octet_count;
            const uint32_t packet_delta = packet_count - _last_sr_packet_count;
            _sender_bitrate_bps = static_cast<double>(octet_delta) * 8.0 /
                                  interval_seconds;
            _sender_packet_rate = static_cast<double>(packet_delta) /
                                  interval_seconds;

            if (clock_rate > 0)
            {
                const uint32_t rtp_delta = rtp_ts - _last_sr_rtp_ts;
                _measured_clock_rate = static_cast<double>(rtp_delta) /
                                       interval_seconds;
                _clock_drift_ppm =
                    (_measured_clock_rate - static_cast<double>(clock_rate)) /
                    static_cast<double>(clock_rate) * 1000000.0;
            }
        }
    }

    ++_sr_count;
    _rtcp_sender_ssrc = sender_ssrc;
    _last_sr_ntp = ntp;
    _last_sr_rtp_ts = rtp_ts;
    _last_sr_packet_count = packet_count;
    _last_sr_octet_count = octet_count;
    _last_sr_receive_ms = receive_ms;
}

RtpRecvStatsBase::ReceiverReport RtpRecvStatsBase::BuildReceiverReport(uint64_t now_ms)
{
    ReceiverReport report;
    report.media_ssrc = _ssrc;
    report.jitter = _jitter;

    if (_has_seq)
    {
        report.extended_highest_seq = _seq_cycles + _max_seq;
        const uint32_t expected = report.extended_highest_seq - _base_seq + 1;
        const int64_t cumulative_lost =
            static_cast<int64_t>(expected) - static_cast<int64_t>(_rtp_packet_count);
        report.cumulative_lost = static_cast<int32_t>(std::clamp<int64_t>(
            cumulative_lost, -0x800000LL, 0x7FFFFFLL));

        const uint32_t expected_interval = expected - _expected_prior;
        const uint32_t received_interval =
            static_cast<uint32_t>(_rtp_packet_count) - _received_prior;
        const int64_t lost_interval =
            static_cast<int64_t>(expected_interval) - received_interval;
        if (expected_interval > 0 && lost_interval > 0)
        {
            const uint64_t fraction =
                (static_cast<uint64_t>(lost_interval) << 8) / expected_interval;
            report.fraction_lost = static_cast<uint8_t>(
                std::min<uint64_t>(fraction, 255));
        }
        _expected_prior = expected;
        _received_prior = static_cast<uint32_t>(_rtp_packet_count);
    }

    if (_last_sr_ntp != 0)
    {
        report.lsr = static_cast<uint32_t>((_last_sr_ntp >> 16) & 0xFFFFFFFFULL);
        if (now_ms >= _last_sr_receive_ms)
        {
            const uint64_t delay =
                (now_ms - _last_sr_receive_ms) * 65536ULL / 1000ULL;
            report.dlsr = static_cast<uint32_t>(
                std::min<uint64_t>(delay, std::numeric_limits<uint32_t>::max()));
        }
    }
    return report;
}


void RtpRecvStatsBase::OnReceiverReport(uint32_t sender_ssrc, uint32_t media_ssrc, uint8_t fraction_lost, int32_t cumulative_lost, uint32_t highest_seq, uint32_t jitter, uint32_t lsr, uint32_t dlsr)
{
    ++_rr_count;
    _rtcp_sender_ssrc = sender_ssrc;
    _rtcp_media_ssrc = media_ssrc;
    _last_rr_fraction_lost = fraction_lost;
    _last_rr_cumulative_lost = cumulative_lost;
    _last_rr_highest_seq = highest_seq;
    _last_rr_lsr = lsr;
    _last_rr_dlsr = dlsr;
    SetJitter(jitter);
}

void RtpRecvStatsBase::SetRttMs(uint32_t rtt_ms)
{
    _rtt_ms = rtt_ms;
}

void RtpRecvStatsBase::SetJitter(uint32_t jitter)
{
    _jitter = jitter;
}

bool RtpHeader::InputFromBuffer(const uint8_t* buf, size_t len)
{
    if (!buf || len < kSize)
    {
        return false;
    }

    const uint8_t vpxcc = buf[0];
    const uint8_t mpt   = buf[1];

    _version      = (vpxcc >> 6)  & 0x03;
    _padding      = ((vpxcc >> 5) & 0x01) != 0;
    _extension    = ((vpxcc >> 4) & 0x01) != 0;
    _csrc         = vpxcc         & 0x0F;

    _marker       = ((mpt >> 7)   & 0x01) != 0;
    _payload_type = mpt           & 0x7F;

    _seq = (static_cast<uint16_t>(buf[2]) << 8) | static_cast<uint16_t>(buf[3]);

    _timestamp = (static_cast<uint32_t>(buf[4]) << 24) | (static_cast<uint32_t>(buf[5]) << 16) | (static_cast<uint32_t>(buf[6]) << 8)  | static_cast<uint32_t>(buf[7]);

    _ssrc = (static_cast<uint32_t>(buf[8]) << 24) | (static_cast<uint32_t>(buf[9]) << 16) | (static_cast<uint32_t>(buf[10]) << 8) | static_cast<uint32_t>(buf[11]);
    return true;
}

void RtpTrack::setInterleavedChannel(uint8_t rtp_channel, uint8_t rtcp_channel)
{
    info_.rtsp_transport.interleaved_rtcp = rtcp_channel;
    info_.rtsp_transport.interleaved_rtp  = rtp_channel;
}

bool AudioTrack::onInputRtp(uint8_t* data, size_t len)
{
    return true;
}

void AudioTrack::onInputRtcp(const uint8_t* data, size_t len)
{
    LOG_INFO("1111");
}

bool VideoTrack::onInputRtp(uint8_t* data, size_t len)
{
    return true;
}

void VideoTrack::onInputRtcp(const uint8_t* data, size_t len)
{
    
}

void RtpPacket::setPayload(const uint8_t* payload, size_t len)
{
    if (!payload || len == 0)
    {
        payload_len_ = 0;
        data_.reset();
        return;
    }

    if (!data_)
    {
        LOG_ERROR("setPayload data_ is null, capacity=", capacity_, " payload_off_=", payload_off_, " len=", len);
    }

    if (payload_off_ > capacity_)
    {
        LOG_ERROR("setPayload invalid payload_off_, payload_off_=", payload_off_, " capacity_=", capacity_);
    }

    if (payload_off_ + len > capacity_)
    {
        LOG_ERROR("setPayload no enough capacity, payload_off_=", payload_off_, " len=", len, " capacity_=", capacity_);
    }

    std::memcpy(data_.get() + payload_off_, payload, len);
    payload_len_ = len;
    size_ = payload_off_ + len;
}

void RtpPacket::setRaw(const uint8_t* data, size_t len)
{
    if(!data || len == 0)
    {
        reset();
        return;
    }

    if (len > getCapacity())
    {
        data_.reset(new uint8_t[len]);
        capacity_ = len;
    }

    memcpy(data_.get(), data, len);
    size_ = len;
}

void RtpPacket::reset()
{
    resetForReuse();
    data_.reset();
    capacity_ = 0;
}

void RtpPacket::resetForReuse()
{
    type_ = TrackInvalid;
    sample_rate_ = 90000;
    ntp_stamp_ms_ = 0;
    track_index_ = -1;

    size_ = 0;

    hdr_len_ = kRtpHeaderSize;
    payload_off_ = kRtpHeaderSize;
    payload_len_ = 0;

    marker_ = false;
    pt_ = 0;
    ts_ = 0;
    ssrc_ = 0;
    seq_ = 0;
    version_ = kRtpVersion;

    padding_ = false;
    extension_ = false;
    cc_ = 0;
    csrc_count_ = 0;
    csrc_.fill(0);

    recv_time_ms_ = 0;
}

bool RtpPacket::reserve(size_t capacity)
{
    if (capacity == 0)
    {
        return false;
    }

    if (capacity_ >= capacity && data_)
    {
        return true;
    }

    std::shared_ptr<uint8_t[]> buf(new uint8_t[capacity], std::default_delete<uint8_t[]>());
    if (!buf)
    {
        return false;
    }

    data_ = std::move(buf);
    capacity_ = capacity;
    size_ = 0;
    return true;
}

void VideoTrack::onOrderedPacket(uint16_t seq, PacketPtr pkt)
{
}
