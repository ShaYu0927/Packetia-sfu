#include "RtpSenderTrack.h"

#include <algorithm>
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

uint32_t CompactNtpNow()
{
    using namespace std::chrono;
    const auto now = system_clock::now().time_since_epoch();
    const auto seconds = duration_cast<std::chrono::seconds>(now);
    const auto nanos = duration_cast<std::chrono::nanoseconds>(now - seconds);

    constexpr uint64_t kNtpUnixEpochOffset = 2208988800ULL;
    const uint64_t ntp_seconds = static_cast<uint64_t>(seconds.count()) + kNtpUnixEpochOffset;
    const uint64_t ntp_fraction = (static_cast<uint64_t>(nanos.count()) << 32) / 1000000000ULL;

    return static_cast<uint32_t>(((ntp_seconds & 0xFFFFULL) << 16) | ((ntp_fraction >> 16) & 0xFFFFULL));
}

uint32_t CalculateRttMs(uint32_t lsr, uint32_t dlsr)
{
    if (lsr == 0)
    {
        return 0;
    }

    const uint32_t arrival = CompactNtpNow();
    const uint32_t rtt_ntp = arrival - lsr - dlsr;
    return static_cast<uint32_t>((static_cast<uint64_t>(rtt_ntp) * 1000ULL + 32768ULL) / 65536ULL);
}

size_t RtpPayloadOffset(const std::vector<uint8_t>& packet)
{
    if (packet.size() < RtpHeader::kSize)
    {
        return packet.size();
    }
    const size_t base = RtpHeader::kSize + static_cast<size_t>(packet[0] & 0x0F) * 4;
    if (packet.size() < base || (packet[0] & 0x10) == 0)
    {
        return base;
    }
    if (packet.size() < base + 4)
    {
        return packet.size();
    }
    const uint16_t words = static_cast<uint16_t>(packet[base + 2] << 8) | packet[base + 3];
    return std::min(packet.size(), base + 4 + static_cast<size_t>(words) * 4);
}
}

RtpSenderTrack::RtpSenderTrack(const RtpSenderTrackConfig& config,
                               std::shared_ptr<IMediaTransport> transport)
    : _config(config),
      _transport(std::move(transport))
{
}

bool RtpSenderTrack::InputRtpPacket(const uint8_t* data, size_t len)
{
    RtpHeader in_header;
    if (!ParseRtpHeader(data, len, in_header))
    {
        return false;
    }

    const auto transport = _transport.lock();
    if (!transport || transport->IsClosed() || !transport->IsWritable())
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

    media::TransportSequenceNumber transport_sequence;
    if (!PrepareTransportCc(packet, transport_sequence))
    {
        return false;
    }

    if (!SendRtpPacket(packet))
    {
        return false;
    }

    
    NotifyPacketSent(transport_sequence, out_seq, packet.size());

    CacheRtpPacket(out_seq, packet);
    ++_packet_count;

    const size_t header_size = RtpPayloadOffset(packet);
    if (packet.size() > header_size)
    {
        _octet_count += packet.size() - header_size;
    }

    return true;
}

void RtpSenderTrack::OnRtcpNack(const std::vector<uint16_t>& lost_seqs)
{
    const auto transport = _transport.lock();
    if (!transport || transport->IsClosed() || !transport->IsWritable() || lost_seqs.empty())
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
        if (_config.max_retransmit_count > 0 && cached.retransmit_count >= _config.max_retransmit_count)
        {
            continue;
        }

        if (cached.last_retransmit_ms != 0 &&
            now_ms >= cached.last_retransmit_ms &&
            now_ms - cached.last_retransmit_ms < _config.min_retransmit_interval_ms)
        {
            continue;
        }

        // RTP sequence 必须保持为对端 NACK 请求的原值；但重传是一次新的
        // 网络发送，因此必须复制缓存包并重新分配 TWCC sequence。
        // 不能直接发送 cached.packet，否则原发送和重传会共用同一个 TWCC 序号。
        std::vector<uint8_t> retransmit_packet = cached.packet;
        media::TransportSequenceNumber transport_sequence;
        if (!PrepareTransportCc(retransmit_packet, transport_sequence))
        {
            continue;
        }

        if (SendRtpPacket(retransmit_packet, true))
        {
            NotifyPacketSent(transport_sequence, seq, retransmit_packet.size());
            cached.last_retransmit_ms = now_ms;
            ++cached.retransmit_count;
        }
    }
}

void RtpSenderTrack::OnRtcpReceiverReport(uint32_t reporter_ssrc, uint32_t media_ssrc, uint8_t fraction_lost, int32_t cumulative_lost, uint32_t highest_seq, uint32_t jitter, uint32_t lsr, uint32_t dlsr)
{
    ++_rr_count;
    _last_rr_reporter_ssrc = reporter_ssrc;
    _last_rr_media_ssrc = media_ssrc;
    _last_rr_fraction_lost = fraction_lost;
    _last_rr_cumulative_lost = cumulative_lost;
    _last_rr_highest_seq = highest_seq;
    _last_rr_jitter = jitter;
    _last_rr_lsr = lsr;
    _last_rr_dlsr = dlsr;
    _rtt_ms = CalculateRttMs(lsr, dlsr);
}

void RtpSenderTrack::OnRtcpPli()
{
    const uint64_t now_ms = NowMs();
    if (_last_pli_ms != 0 && now_ms >= _last_pli_ms && now_ms - _last_pli_ms < _pli_interval_ms)
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

void RtpSenderTrack::SetPacketSentCallback(PacketSentCallback cb)
{
    _packet_sent_cb = std::move(cb);
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

        const uint16_t ext_words = (static_cast<uint16_t>(data[header_size + 2]) << 8) | static_cast<uint16_t>(data[header_size + 3]);
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

bool RtpSenderTrack::RewriteRtpPacket(std::vector<uint8_t>& packet, const RtpHeader& in_header, uint16_t& out_seq, uint32_t& out_timestamp)
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

bool RtpSenderTrack::WriteTransportCcExtension(std::vector<uint8_t>& packet,
                                                uint16_t transport_sequence) const
{
    // extension ID 来自 SDP a=extmap 协商。0 表示未启用，1~14 是 RFC 8285
    // one-byte header 可使用的 ID；绝不能在这里写死成某个固定数字。
    const uint8_t extension_id = _config.transport_cc_extension_id;
    if (extension_id == 0 || extension_id > 14 || packet.size() < RtpHeader::kSize)
    {
        return false;
    }

    const size_t base_header_size = RtpHeader::kSize + static_cast<size_t>(packet[0] & 0x0F) * 4;
    if (packet.size() < base_header_size)
    {
        return false;
    }

    if ((packet[0] & 0x10) == 0)
    {
        // 新建 RFC 8285 one-byte 扩展块：4 字节块头 + 3 字节元素 + 1 字节 padding。
        const uint8_t extension[8] = {
            0xBE, 0xDE, 0x00, 0x01,
            static_cast<uint8_t>((extension_id << 4) | 0x01),
            static_cast<uint8_t>(transport_sequence >> 8),
            static_cast<uint8_t>(transport_sequence),
            0x00};
        packet.insert(packet.begin() + static_cast<std::ptrdiff_t>(base_header_size),
                      extension, extension + sizeof(extension));
        packet[0] |= 0x10;
        return true;
    }

    if (packet.size() < base_header_size + 4)
    {
        return false;
    }
    const uint16_t profile = static_cast<uint16_t>(packet[base_header_size] << 8) |
                             packet[base_header_size + 1];
    if (profile != 0xBEDE)
    {
        // 不擅自改写 two-byte 或应用自定义扩展格式。
        return false;
    }

    const uint16_t words = static_cast<uint16_t>(packet[base_header_size + 2] << 8) |
                           packet[base_header_size + 3];
    const size_t extension_data = base_header_size + 4;
    const size_t extension_end = extension_data + static_cast<size_t>(words) * 4;
    if (packet.size() < extension_end)
    {
        return false;
    }

    // RTP 已经带扩展时逐项扫描。已有同 ID 元素就原地更新，常见于从
    // 缓存复制出来的 RTX/NACK 重传包，避免一个 RTP 包出现两个 TWCC 元素。
    for (size_t offset = extension_data; offset < extension_end;)
    {
        const uint8_t header = packet[offset];
        if (header == 0)
        {
            ++offset;
            continue;
        }
        const uint8_t id = header >> 4;
        if (id == 15)
        {
            break;
        }
        const size_t element_size = static_cast<size_t>(header & 0x0F) + 1;
        if (offset + 1 + element_size > extension_end)
        {
            return false;
        }
        if (id == extension_id)
        {
            if (element_size != 2)
            {
                return false;
            }
            WriteUint16(packet.data() + offset + 1, transport_sequence);
            return true;
        }
        offset += 1 + element_size;
    }

    // 包里存在 MID、audio-level 等其他 one-byte 扩展，但还没有 TWCC：
    // 保留所有原扩展，在扩展数据末尾增加一个 4 字节 word。
    const uint8_t element[4] = {
        static_cast<uint8_t>((extension_id << 4) | 0x01),
        static_cast<uint8_t>(transport_sequence >> 8),
        static_cast<uint8_t>(transport_sequence),
        0x00};
    packet.insert(packet.begin() + static_cast<std::ptrdiff_t>(extension_end),
                  element, element + sizeof(element));
    WriteUint16(packet.data() + base_header_size + 2, static_cast<uint16_t>(words + 1));
    return true;
}

bool RtpSenderTrack::PrepareTransportCc(std::vector<uint8_t>& packet,
                                        media::TransportSequenceNumber& sequence) const
{
    // 没有通过 SDP 启用 transport-cc 时保持原发送行为，也不触发发送历史回调。
    if (_config.transport_cc_extension_id == 0)
    {
        sequence = {};
        sequence.extended_sequence = -1;
        return true;
    }
    if (!_config.transport_sequence_allocator)
    {
        return false;
    }

    // 先用占位值验证并建立扩展结构，再从共享 allocator 取正式序号。
    // 这样遇到不支持的扩展格式时不会白白消耗序号，也就不会在接收端
    // 形成一个从未实际发送过、却被误认为丢失的 transport sequence。
    if (!WriteTransportCcExtension(packet, 0))
    {
        return false;
    }
    sequence = _config.transport_sequence_allocator->Allocate();
    return WriteTransportCcExtension(packet, sequence.wire_sequence);
}

void RtpSenderTrack::NotifyPacketSent(const media::TransportSequenceNumber& sequence,
                                      uint16_t rtp_sequence,
                                      size_t packet_size)
{
    // extended_sequence 是本地连续键；wire_sequence 是 RTP 中的 16 位值。
    // 两者必须一起保留，后续反馈适配器负责把回绕后的 wire sequence
    // 展开并匹配到正确的 PacketHistory 记录。
    if (_packet_sent_cb && sequence.extended_sequence >= 0)
    {
        _packet_sent_cb(sequence.extended_sequence,
                        sequence.wire_sequence,
                        _config.local_ssrc,
                        rtp_sequence,
                        NowMs(),
                        static_cast<uint32_t>(packet_size));
    }
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

bool RtpSenderTrack::SendRtpPacket(const std::vector<uint8_t>& packet,
                                   bool retransmit)
{
    const auto transport = _transport.lock();
    if (!transport || packet.empty() || !transport->IsWritable())
    {
        return false;
    }

    return transport->SendRtp(packet.data(), packet.size(), retransmit) == SendResult::Ok;
}

}
