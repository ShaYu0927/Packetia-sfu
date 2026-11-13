#include "RtpSenderTrack.h"
#include "MediaLatency.h"
#include "SendSideController.h"

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

size_t RtpPayloadOffset(const common::ByteVector& packet)
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

RtpSenderTrack::RtpSenderTrack(const RtpSenderTrackConfig& config, std::shared_ptr<IMediaTransport> transport, Clock clock)
    : _config(config),
      _clock(clock ? std::move(clock) : Clock{NowMs}),
      _next_rtx_sequence(config.initial_rtx_sequence),
      _retransmission_cache({config.rtp_cache_size, config.max_retransmit_age_ms,
                             config.min_retransmit_interval_ms, config.max_retransmit_count}),
      _transport(std::move(transport))
{
    if (auto bound_transport = _transport.lock())
    {
        _send_controller = bound_transport->GetSendSideController();
        if (_send_controller)
        {
            _config.transport_sequence_allocator = _send_controller->SequenceAllocator();
            _send_controller->RegisterSender(_config.local_ssrc,
                _config.sample_rate > 0 ? static_cast<uint32_t>(_config.sample_rate) : 0);
        }
    }
}

RtpSenderTrack::~RtpSenderTrack()
{
    if (_send_controller) _send_controller->UnregisterSender(_config.local_ssrc);
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

    common::ByteVector packet(data, data + len);
    uint16_t out_seq = 0;
    uint32_t out_timestamp = 0;
    int64_t source_timestamp = 0;
    if (!_forwarding.Prepare(in_header.getSequence(), in_header.getTimestamp(),
                             out_seq, out_timestamp, source_timestamp) ||
        !RewriteRtpPacket(packet, in_header, out_seq, out_timestamp)) return false;

    media::TransportSequenceNumber transport_sequence;
    if (_config.rewrite_header_extensions && !RewriteHeaderExtensions(packet)) return false;
    if (!PrepareTransportCc(packet, transport_sequence))
    {
        return false;
    }

    if (!SendRtpPacket(packet, false, transport_sequence, out_seq))
    {
        return false;
    }

    
    _forwarding.OnMediaSent(out_seq, out_timestamp, source_timestamp);

    _retransmission_cache.Store(out_seq, packet.data(), packet.size(), _clock());
    ++_packet_count;

    const size_t header_size = RtpPayloadOffset(packet);
    if (packet.size() > header_size)
    {
        _octet_count += packet.size() - header_size;
    }

    return true;
}

void RtpSenderTrack::BeginDiscontinuity()
{
    _retransmission_cache.Clear();
    _forwarding.BeginDiscontinuity();
}

bool RtpSenderTrack::SendPadding(size_t padding_bytes, int probe_cluster_id)
{
    if (!_config.transport_cc_extension_id || !padding_bytes || padding_bytes > 255) return false;
    common::ByteVector packet(12 + padding_bytes, 0);
    packet[0] = 0xa0;
    packet[1] = _config.payload_type;
    const auto sequence = _forwarding.NextPaddingSequence();
    WriteUint16(packet.data() + 2, sequence);
    WriteUint32(packet.data() + 4, _forwarding.LastTimestamp());
    WriteUint32(packet.data() + 8, _config.local_ssrc);
    packet.back() = uint8_t(padding_bytes);
    if (_config.rewrite_header_extensions && !RewriteHeaderExtensions(packet)) return false;
    media::TransportSequenceNumber transport_sequence;
    if (!PrepareTransportCc(packet, transport_sequence) ||
        !SendRtpPacket(packet, false, transport_sequence, sequence, probe_cluster_id, media::MediaKind::Padding)) return false;
    // Padding occupies downstream sequence space, but no source sequence.
    _forwarding.OnPaddingSent(sequence);
    ++_packet_count;
    return true;
}

void RtpSenderTrack::OnRtcpNack(const std::vector<uint16_t>& lost_seqs)
try
{
    const auto transport = _transport.lock();
    if (!transport || transport->IsClosed() || !transport->IsWritable() || lost_seqs.empty())
    {
        return;
    }

    const uint64_t now_ms = _clock();
    _retransmission_cache.Retransmit(lost_seqs, now_ms,
        [this](uint16_t seq, const common::SharedBuffer& cached) {
            // Each retransmission gets a new transport sequence, while the
            // original RTP sequence remains the one requested by the peer.
            common::ByteVector packet(cached.Data(), cached.Data() + cached.Size());
            media::TransportSequenceNumber transport_sequence;
            if (!PrepareTransportCc(packet, transport_sequence) ||
                !SendRtpPacket(packet, true, transport_sequence, seq))
                return false;
            return true;
        });
}

catch (const std::bad_alloc&) {} // Retry on subsequent feedback when capacity is available.

void RtpSenderTrack::OnRtcpReceiverReport(uint32_t reporter_ssrc, uint32_t media_ssrc, uint8_t fraction_lost, int32_t cumulative_lost, uint32_t highest_seq, uint32_t jitter, uint32_t lsr, uint32_t dlsr)
{
    ++_feedback.rr_count;
    _feedback.reporter_ssrc = reporter_ssrc;
    _feedback.media_ssrc = media_ssrc;
    _feedback.fraction_lost = fraction_lost;
    _feedback.cumulative_lost = cumulative_lost;
    _feedback.highest_seq = highest_seq;
    _feedback.jitter = jitter;
    _feedback.lsr = lsr;
    _feedback.dlsr = dlsr;
    _feedback.rtt_ms = CalculateRttMs(lsr, dlsr);
}

void RtpSenderTrack::OnRtcpPli()
{
    if (!_feedback.AllowKeyFrameRequest(_clock())) return;
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

bool RtpSenderTrack::RewriteRtpPacket(common::ByteVector& packet, const RtpHeader& in_header, uint16_t& out_seq, uint32_t& out_timestamp)
{
    if (packet.size() < RtpHeader::kSize)
    {
        return false;
    }

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

bool RtpSenderTrack::RewriteHeaderExtensions(common::ByteVector& packet)
{
    const size_t base = RtpHeader::kSize + 4 * (packet[0] & 15);
    if (packet.size() < base) return false;
    if (packet[0] & 0x10) {
        if (packet.size() < base + 4) return false;
        const size_t end = base + 4 + 4 * ((packet[base + 2] << 8) | packet[base + 3]);
        if (end > packet.size()) return false;
        packet.erase(packet.begin() + base, packet.begin() + end);
        packet[0] &= ~0x10;
    }
    if (!_config.mid_extension_id) return _config.mid.empty();
    if (_config.mid_extension_id > 14 || _config.mid.empty() || _config.mid.size() > 16 ||
        _config.mid_extension_id == _config.transport_cc_extension_id) return false;
    common::ByteVector extension{0xBE, 0xDE, 0, 0,
        static_cast<uint8_t>((_config.mid_extension_id << 4) | (_config.mid.size() - 1))};
    extension.insert(extension.end(), _config.mid.begin(), _config.mid.end());
    while (extension.size() % 4) extension.push_back(0);
    WriteUint16(extension.data() + 2, static_cast<uint16_t>((extension.size() - 4) / 4));
    packet.insert(packet.begin() + base, extension.begin(), extension.end());
    packet[0] |= 0x10;
    return true;
}

bool RtpSenderTrack::WriteTransportCcExtension(common::ByteVector& packet,
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

bool RtpSenderTrack::PrepareTransportCc(common::ByteVector& packet, media::TransportSequenceNumber& sequence) const
{
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

    if (!WriteTransportCcExtension(packet, 0))
    {
        return false;
    }
    sequence = _config.transport_sequence_allocator->Allocate();
    return WriteTransportCcExtension(packet, sequence.wire_sequence);
}

bool RtpSenderTrack::SendRtpPacket(const common::ByteVector& packet, bool retransmit,
                                   const media::TransportSequenceNumber& sequence, uint16_t rtp_sequence,
                                   int probe_cluster_id, media::MediaKind kind)
{
    const auto transport = _transport.lock();
    if (!transport || packet.empty() || !transport->IsWritable()) return false;

    std::function<void()> on_sent;
    if (_config.transport_cc_extension_id != 0 && sequence.extended_sequence >= 0)
    {
        media::PacketSendInfo info;
        info.transport_sequence = sequence.extended_sequence;
        info.ssrc = _config.local_ssrc;
        info.rtp_sequence = rtp_sequence;
        info.size_bytes = static_cast<uint32_t>(packet.size());
        info.media_kind = kind;
        info.is_probe = probe_cluster_id >= 0;
        info.probe_cluster_id = probe_cluster_id;
        const auto controller = _send_controller;
        const auto epoch = controller ? controller->PathEpoch() : 0;
        const auto clock = _clock;
        const auto callback = _packet_sent_cb;
        on_sent = [controller, epoch, clock, callback, info, sequence]() mutable {
            info.send_time_ms = clock();
            if (controller && !controller->OnPacketSentForPath(info, epoch)) return;
            if (callback) callback(sequence.extended_sequence, sequence.wire_sequence,
                info.ssrc, info.rtp_sequence, info.send_time_ms, info.size_bytes);
        };
    }
    media_latency::SendScope trace_scope(retransmit);
    return transport->SendRtpWithCompletion(packet.data(), packet.size(), retransmit,
                                             std::move(on_sent)) == SendResult::Ok;
}

}
