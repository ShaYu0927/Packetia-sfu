#include "Rtp.h"
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

void RtpRecvStatsBase::OnSenderReport(uint32_t sender_ssrc, uint64_t ntp, uint32_t rtp_ts, uint32_t packet_count, uint32_t octet_count)
{
    ++_sr_count;
    _rtcp_sender_ssrc = sender_ssrc;
    _last_sr_ntp = ntp;
    _last_sr_rtp_ts = rtp_ts;
    _last_sr_packet_count = packet_count;
    _last_sr_octet_count = octet_count;
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
