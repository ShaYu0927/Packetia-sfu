#include "H26xPacketBuffer.h"

#include <utility>

namespace media
{
namespace
{
bool TimestampAheadOrAt(uint32_t lhs, uint32_t rhs)
{
    return static_cast<int32_t>(lhs - rhs) >= 0;
}
}

void RtpSequenceNumberUnwrapper::Reset()
{
    initialized_ = false;
    latest_unwrapped_seq_ = 0;
}

int64_t RtpSequenceNumberUnwrapper::Unwrap(uint16_t sequence_number)
{
    if (!initialized_)
    {
        initialized_ = true;
        latest_unwrapped_seq_ = sequence_number;
        return latest_unwrapped_seq_;
    }
    static constexpr int64_t kModulo = 1LL << 16;
    static constexpr int64_t kHalf = 1LL << 15;
    const int64_t cycle_base = latest_unwrapped_seq_ & ~0xFFFFLL;
    int64_t candidate = cycle_base + sequence_number;
    if (candidate - latest_unwrapped_seq_ > kHalf)
        candidate -= kModulo;
    else if (latest_unwrapped_seq_ - candidate > kHalf)
        candidate += kModulo;
    if (candidate > latest_unwrapped_seq_)
        latest_unwrapped_seq_ = candidate;
    return candidate;
}

H264PacketBuffer::H264PacketBuffer() : H264PacketBuffer(Config{}) {}

H264PacketBuffer::H264PacketBuffer(const Config& config) : config_(config)
{
    if (config_.max_frame_packets == 0 || config_.max_frame_packets > kBufferSize)
        config_.max_frame_packets = kBufferSize;
}

void H264PacketBuffer::Reset()
{
    ClearInternal();
    seq_unwrapper_.Reset();
}

void H264PacketBuffer::ClearInternal()
{
    for (auto& packet : buffer_)
        packet.reset();
    last_continuous_unwrapped_seq_.reset();
    buffered_packet_count_ = 0;
}

size_t H264PacketBuffer::BufferIndex(int64_t unwrapped_seq)
{
    return static_cast<size_t>(unwrapped_seq) & (kBufferSize - 1);
}

H264PacketBuffer::BufferedPacket* H264PacketBuffer::GetPacket(int64_t seq)
{
    auto& packet = buffer_[BufferIndex(seq)];
    return packet && packet->unwrapped_seq == seq ? packet.get() : nullptr;
}

const H264PacketBuffer::BufferedPacket* H264PacketBuffer::GetPacket(int64_t seq) const
{
    const auto& packet = buffer_[BufferIndex(seq)];
    return packet && packet->unwrapped_seq == seq ? packet.get() : nullptr;
}

bool H264PacketBuffer::BeginningOfStream(const H264ParsedPacket& packet) const
{
    if (packet.has_sps)
        return true;
    return config_.idr_only_keyframes_allowed && packet.has_idr &&
           !packet.units.empty() &&
           (packet.units.front().unit_type != H264PayloadUnitType::FuAFragment ||
            packet.units.front().fu_start);
}

bool H264PacketBuffer::CanAdvanceFrom(int64_t inserted_seq, const H264ParsedPacket& packet)
{
    if (last_continuous_unwrapped_seq_)
    {
        if (inserted_seq <= *last_continuous_unwrapped_seq_)
        {
            return false;
        }
        if (inserted_seq == *last_continuous_unwrapped_seq_ + 1)
        {
            return true;
        }
    }

    if (!BeginningOfStream(packet))
    {
        return false;
    }

    // A keyframe start is a valid resynchronization point after a sequence gap.
    last_continuous_unwrapped_seq_ = inserted_seq - 1;
    return true;
}

int64_t H264PacketBuffer::FindFrameStart(int64_t end_seq) const
{
    const auto* end = GetPacket(end_seq);
    if (!end)
    {
        return end_seq;
    }

    const int64_t earliest = end_seq - static_cast<int64_t>(config_.max_frame_packets) + 1;
    int64_t start_seq = end_seq;
    while (start_seq > earliest)
    {
        const auto* previous = GetPacket(start_seq - 1);
        if (!previous ||
            previous->packet.timestamp != end->packet.timestamp ||
            previous->packet.ssrc != end->packet.ssrc)
        {
            break;
        }
        --start_seq;
    }
    return start_seq;
}

H264PacketBuffer::InsertResult H264PacketBuffer::InsertPacket(H264ParsedPacket packet)
{
    InsertResult result;
    if (!packet.valid || packet.malformed)
        return result;

    const int64_t unwrapped_seq = seq_unwrapper_.Unwrap(packet.seq);
    if (last_continuous_unwrapped_seq_ && unwrapped_seq <= *last_continuous_unwrapped_seq_)
    {
        result.late = true;
        return result;
    }

    auto& slot = buffer_[BufferIndex(unwrapped_seq)];
    if (slot)
    {
        if (slot->unwrapped_seq == unwrapped_seq)
        {
            result.duplicate = true;
            return result;
        }
        if (TimestampAheadOrAt(slot->packet.timestamp, packet.timestamp))
        {
            result.late = true;
            return result;
        }

        result.buffer_collision = true;
        slot.reset();
        --buffered_packet_count_;
    }

    auto stored = std::make_unique<BufferedPacket>();
    stored->unwrapped_seq = unwrapped_seq;
    stored->packet = std::move(packet);
    buffer_[BufferIndex(unwrapped_seq)] = std::move(stored);
    ++buffered_packet_count_;
    result.inserted = true;
    FindFrames(unwrapped_seq, result);
    return result;
}

void H264PacketBuffer::FindFrames(int64_t inserted_seq, InsertResult& result)
{
    auto* packet = GetPacket(inserted_seq);
    if (!packet || !CanAdvanceFrom(inserted_seq, packet->packet))
    {
        return;
    }

    for (size_t offset = 0; offset < kBufferSize; ++offset)
    {
        const int64_t seq = inserted_seq + static_cast<int64_t>(offset);
        packet = GetPacket(seq);
        if (!packet)
        {
            return;
        }

        last_continuous_unwrapped_seq_ = seq;
        if (!packet->packet.marker)
        {
            continue;
        }

        H264AccessUnit frame;
        if (!MaybeAssembleFrame(FindFrameStart(seq), seq, frame))
        {
            return;
        }
        result.frames.emplace_back(std::move(frame));
    }
}

bool H264PacketBuffer::MaybeAssembleFrame(int64_t start_seq, int64_t end_seq, H264AccessUnit& frame)
{
    if (end_seq < start_seq ||
        static_cast<uint64_t>(end_seq - start_seq + 1) > config_.max_frame_packets)
        return false;
    auto* first = GetPacket(start_seq);
    if (!first)
        return false;

    frame.ssrc = first->packet.ssrc;
    frame.timestamp = first->packet.timestamp;
    frame.first_seq = static_cast<uint16_t>(start_seq);
    frame.last_seq = static_cast<uint16_t>(end_seq);
    frame.marker = true;

    bool validating_fu = false;
    size_t unit_count = 0;
    size_t payload_bytes = 0;
    for (int64_t seq = start_seq; seq <= end_seq; ++seq)
    {
        auto* buffered = GetPacket(seq);
        if (!buffered || buffered->packet.ssrc != frame.ssrc ||
            buffered->packet.timestamp != frame.timestamp)
            return false;
        frame.keyframe = frame.keyframe || buffered->packet.has_key_nalu;
        frame.has_sps = frame.has_sps || buffered->packet.has_sps;
        frame.has_pps = frame.has_pps || buffered->packet.has_pps;
        frame.has_idr = frame.has_idr || buffered->packet.has_idr;
        unit_count += buffered->packet.units.size();
        for (const auto& unit : buffered->packet.units)
        {
            payload_bytes += unit.data.size();
        }
        if (!ValidatePacketUnits(buffered->packet, validating_fu))
        {
            return false;
        }
    }
    if (validating_fu)
    {
        return false;
    }
    if (frame.has_idr && !config_.idr_only_keyframes_allowed &&
        (!frame.has_sps || !frame.has_pps))
    {
        return false;
    }

    bool fu_active = false;
    std::vector<uint8_t> fu_nalu;
    frame.nalus.reserve(unit_count);
    fu_nalu.reserve(payload_bytes + 1);
    for (int64_t seq = start_seq; seq <= end_seq; ++seq)
    {
        auto* buffered = GetPacket(seq);
        if (!AppendPacketUnits(buffered->packet, fu_active, fu_nalu, frame))
        {
            return false;
        }
    }
    if (fu_active || frame.nalus.empty())
    {
        return false;
    }

    frame.complete = true;
    RemovePackets(start_seq, end_seq);
    return true;
}

void H264PacketBuffer::RemovePackets(int64_t start_seq, int64_t end_seq)
{
    for (int64_t seq = start_seq; seq <= end_seq; ++seq)
    {
        auto& packet = buffer_[BufferIndex(seq)];
        if (packet && packet->unwrapped_seq == seq)
        {
            packet.reset();
            --buffered_packet_count_;
        }
    }
}

bool H264PacketBuffer::ValidatePacketUnits(const H264ParsedPacket& packet,
                                           bool& fu_active) const
{
    for (const auto& unit : packet.units)
    {
        if (unit.unit_type == H264PayloadUnitType::SingleNalu ||
            unit.unit_type == H264PayloadUnitType::StapANalu)
        {
            if (fu_active || unit.data.empty())
                return false;
            continue;
        }
        if (unit.unit_type != H264PayloadUnitType::FuAFragment || unit.data.empty())
            return false;
        if (unit.fu_start)
        {
            if (fu_active)
                return false;
            fu_active = true;
        }
        else if (!fu_active)
            return false;
        if (unit.fu_end)
            fu_active = false;
    }
    return true;
}

bool H264PacketBuffer::AppendPacketUnits(H264ParsedPacket& packet, bool& fu_active,
                                         std::vector<uint8_t>& fu_nalu,
                                         H264AccessUnit& frame)
{
    for (auto& unit : packet.units)
    {
        if (unit.unit_type == H264PayloadUnitType::SingleNalu ||
            unit.unit_type == H264PayloadUnitType::StapANalu)
        {
            if (fu_active || unit.data.empty())
                return false;
            frame.nalus.emplace_back(std::move(unit.data));
            continue;
        }
        if (unit.unit_type != H264PayloadUnitType::FuAFragment || unit.data.empty())
            return false;
        if (unit.fu_start)
        {
            if (fu_active)
                return false;
            fu_active = true;
            fu_nalu.clear();
            fu_nalu.push_back(unit.reconstructed_nal_header);
        }
        else if (!fu_active)
            return false;
        fu_nalu.insert(fu_nalu.end(), unit.data.begin(), unit.data.end());
        if (unit.fu_end)
        {
            frame.nalus.emplace_back(std::move(fu_nalu));
            fu_nalu.clear();
            fu_active = false;
        }
    }
    return true;
}

} // namespace media
