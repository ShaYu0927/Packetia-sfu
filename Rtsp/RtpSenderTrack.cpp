#include "RtpSenderTrack.h"

#include <chrono>
#include <utility>

namespace rtsp
{
namespace
{
uint64_t NowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void WriteUint16(uint8_t* out, uint16_t value)
{
    out[0] = static_cast<uint8_t>((value >> 8) & 0xFF);
    out[1] = static_cast<uint8_t>(value & 0xFF);
}

void WriteUint32(uint8_t* out, uint32_t value)
{
    out[0] = static_cast<uint8_t>((value >> 24) & 0xFF);
    out[1] = static_cast<uint8_t>((value >> 16) & 0xFF);
    out[2] = static_cast<uint8_t>((value >> 8) & 0xFF);
    out[3] = static_cast<uint8_t>(value & 0xFF);
}
}

RtpSenderTrack::RtpSenderTrack(const RtpSenderTrackConfig& config, IPacketSender* sender)
    : _config(config),
      _sender(sender)
{
}

bool RtpSenderTrack::InputRtpPacket(const uint8_t* data, size_t len)
{
    RtpHeader in_header;
    if (!ParseRtpHeader(data, len, in_header))
    {
        return false;
    }

    if (!_sender || _sender->IsClosed() || !_sender->IsWritable())
    {
        return false;
    }

    std::vector<uint8_t> packet(data, data + len);

    uint16_t out_seq = 0;
    uint32_t out_timestamp = 0;
    if (!RewriteRtpPacket(packet, in_header, out_seq, out_timestamp))
    {
        return false;
    }

    if (!SendRtpPacket(packet, out_seq))
    {
        return false;
    }

    CacheRtpPacket(out_seq, packet);
    ++_packet_count;

    const size_t header_size = in_header.getHeaderSize();
    if (packet.size() > header_size)
    {
        _octet_count += packet.size() - header_size;
    }

    return true;
}

void RtpSenderTrack::OnRtcpNack(const std::vector<uint16_t>& lost_seqs)
{
    if (!_sender || _sender->IsClosed() || lost_seqs.empty())
    {
        return;
    }

    const uint64_t now_ms = NowMs();

    for (uint16_t seq : lost_seqs)
    {
        auto it = _rtp_cache.find(seq);
        if (it == _rtp_cache.end())
        {
            continue;
        }

        CachedRtpPacket& cached = it->second;
        if (_config.max_retransmit_count > 0 &&
            cached.retransmit_count >= _config.max_retransmit_count)
        {
            continue;
        }

        if (cached.last_retransmit_ms != 0 &&
            now_ms >= cached.last_retransmit_ms &&
            now_ms - cached.last_retransmit_ms < _config.min_retransmit_interval_ms)
        {
            continue;
        }

        if (SendRtpPacket(cached.packet, seq))
        {
            cached.last_retransmit_ms = now_ms;
            ++cached.retransmit_count;
        }
    }
}

void RtpSenderTrack::OnRtcpPli()
{
    const uint64_t now_ms = NowMs();
    if (_last_pli_ms != 0 &&
        now_ms >= _last_pli_ms &&
        now_ms - _last_pli_ms < _pli_interval_ms)
    {
        return;
    }

    _last_pli_ms = now_ms;
    if (_keyframe_cb)
    {
        _keyframe_cb();
    }
}

void RtpSenderTrack::OnRtcpFir()
{
    OnRtcpPli();
}

void RtpSenderTrack::Tick(uint64_t now_ms)
{
    _last_tick_ms = now_ms;
}

void RtpSenderTrack::SetKeyFrameRequestCallback(KeyFrameRequestCallback cb)
{
    _keyframe_cb = std::move(cb);
}

bool RtpSenderTrack::ParseRtpHeader(const uint8_t* data, size_t len, RtpHeader& header)
{
    if (!header.InputFromBuffer(data, len))
    {
        return false;
    }

    if (header.getVersion() != 2)
    {
        return false;
    }

    if (len < header.getHeaderSize())
    {
        return false;
    }

    if (header.getExtension())
    {
        const size_t header_size = header.getHeaderSize();
        if (len < header_size + 4)
        {
            return false;
        }

        const uint16_t ext_words =
            (static_cast<uint16_t>(data[header_size + 2]) << 8) |
            static_cast<uint16_t>(data[header_size + 3]);
        const size_t ext_size = 4 + static_cast<size_t>(ext_words) * 4;
        if (len < header_size + ext_size)
        {
            return false;
        }
    }

    if (header.getPadding())
    {
        const uint8_t padding = data[len - 1];
        if (padding == 0 || padding > len - header.getHeaderSize())
        {
            return false;
        }
    }

    return true;
}

bool RtpSenderTrack::RewriteRtpPacket(std::vector<uint8_t>& packet,
                                      const RtpHeader& in_header,
                                      uint16_t& out_seq,
                                      uint32_t& out_timestamp)
{
    if (packet.size() < RtpHeader::kSize)
    {
        return false;
    }

    out_seq = RewriteSeq(in_header.getSequence());
    out_timestamp = RewriteTimestamp(in_header.getTimestamp());

    uint8_t payload_type = in_header.getPayloadType();
    if (_config.rewrite_payload_type)
    {
        payload_type = static_cast<uint8_t>(_config.payload_type & 0x7F);
    }

    packet[1] = static_cast<uint8_t>((packet[1] & 0x80) | payload_type);
    WriteUint16(packet.data() + 2, out_seq);
    WriteUint32(packet.data() + 4, out_timestamp);
    WriteUint32(packet.data() + 8, _config.local_ssrc);

    return true;
}

uint16_t RtpSenderTrack::RewriteSeq(uint16_t in_seq)
{
    if (!_started)
    {
        _base_in_seq = in_seq;
        _base_out_seq = in_seq;
    }

    return static_cast<uint16_t>(_base_out_seq + static_cast<uint16_t>(in_seq - _base_in_seq));
}

uint32_t RtpSenderTrack::RewriteTimestamp(uint32_t in_timestamp)
{
    if (!_started)
    {
        _base_in_timestamp = in_timestamp;
        _base_out_timestamp = in_timestamp;
        _started = true;
    }

    return _base_out_timestamp + (in_timestamp - _base_in_timestamp);
}

void RtpSenderTrack::CacheRtpPacket(uint16_t out_seq, const std::vector<uint8_t>& packet)
{
    if (_config.rtp_cache_size == 0)
    {
        return;
    }

    auto it = _rtp_cache.find(out_seq);
    if (it == _rtp_cache.end())
    {
        _cache_order.push_back(out_seq);
    }

    CachedRtpPacket cached;
    cached.packet = packet;
    _rtp_cache[out_seq] = std::move(cached);

    while (_rtp_cache.size() > _config.rtp_cache_size && !_cache_order.empty())
    {
        uint16_t old_seq = _cache_order.front();
        _cache_order.pop_front();
        _rtp_cache.erase(old_seq);
    }
}

bool RtpSenderTrack::SendRtpPacket(const std::vector<uint8_t>& packet, uint16_t out_seq)
{
    if (!_sender || packet.empty())
    {
        return false;
    }

    SendOptions options;
    options.type = PacketType::Rtp;
    options.allow_queue = true;
    options.ssrc = _config.local_ssrc;
    options.seq = out_seq;

    return _sender->SendPacket(packet.data(), packet.size(), options) == SendResult::Ok;
}

}
