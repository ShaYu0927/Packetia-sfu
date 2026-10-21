#include "H26xPacketBuffer.h"

#include <algorithm>
#include <utility>

namespace media
{
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
    arrivals_.clear();
    last_continuous_unwrapped_seq_.reset();
    last_continuous_timestamp_.reset();
    current_frame_start_.reset();
    current_frame_timestamp_ = 0;
    discarding_frame_ = false;
    waiting_for_recovery_ = false;
    buffered_packet_count_ = 0;
}

void H264PacketBuffer::ErasePacket(std::unique_ptr<BufferedPacket>& packet)
{
    arrivals_.erase(packet->arrival);
    packet.reset();
    --buffered_packet_count_;
}

size_t H264PacketBuffer::FlushExpired(TimePoint now)
{
    if (arrivals_.empty() || now < arrivals_.begin()->first ||
        now - arrivals_.begin()->first < std::chrono::milliseconds(config_.max_wait_ms))
        return 0;
    const size_t discarded = buffered_packet_count_;
    // Retain a sequence floor across recovery so a late retransmission cannot
    // revive an abandoned AU. Reset(), in contrast, starts a new stream.
    auto floor = last_continuous_unwrapped_seq_;
    auto timestamp = last_continuous_timestamp_;
    for (const auto& entry : arrivals_) {
        if (!floor || entry.second >= *floor) {
            floor = entry.second;
            timestamp = GetPacket(entry.second)->packet.timestamp;
        }
    }
    ClearInternal();
    last_continuous_unwrapped_seq_ = floor;
    last_continuous_timestamp_ = timestamp;
    waiting_for_recovery_ = true;
    return discarded;
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
    for (const auto& unit : packet.units)
    {
        if (unit.unit_type == H264PayloadUnitType::FuAFragment && !unit.fu_start)
            continue;
        if (unit.is_sps)
            return true;
        if (config_.idr_only_keyframes_allowed && unit.is_idr && unit.first_slice)
            return true;
    }
    return false;
}

bool H264PacketBuffer::CanAdvanceFrom(int64_t inserted_seq, const H264ParsedPacket& packet)
{
    if (last_continuous_unwrapped_seq_)
    {
        if (inserted_seq <= *last_continuous_unwrapped_seq_)
        {
            return false;
        }
        if (!waiting_for_recovery_ && inserted_seq == *last_continuous_unwrapped_seq_ + 1)
        {
            return true;
        }
        // Another IDR slice/FU start within this AU is not a recovery point.
        // Wait for the missing sequence rather than emitting only its suffix.
        if (last_continuous_timestamp_ == packet.timestamp)
            return false;
    }

    if (!BeginningOfStream(packet))
    {
        return false;
    }

    // Recovery commits to a verified first IDR slice (or SPS prefix).
    // Packets from the abandoned AU can no longer be repaired after this point.
    RemovePacketsBefore(inserted_seq);
    current_frame_start_.reset();
    discarding_frame_ = false;
    last_continuous_unwrapped_seq_ = inserted_seq - 1;
    waiting_for_recovery_ = false;
    return true;
}

H264PacketBuffer::InsertResult H264PacketBuffer::InsertPacket(H264ParsedPacket packet, TimePoint now)
{
    InsertResult result;
    if (!packet.valid || packet.malformed)
        return result;

    result.expired_packets = FlushExpired(now);

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
        // RTP timestamps are presentation times and can move backwards with
        // B pictures. Ring ownership is determined only by extended sequence.
        if (slot->unwrapped_seq > unwrapped_seq)
        {
            result.late = true;
            return result;
        }

        result.buffer_collision = true;
        ErasePacket(slot);
    }

    auto stored = std::make_unique<BufferedPacket>();
    stored->unwrapped_seq = unwrapped_seq;
    stored->packet = std::move(packet);
    stored->arrival = arrivals_.emplace(now, unwrapped_seq);
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
        last_continuous_timestamp_ = packet->packet.timestamp;
        if (current_frame_start_ && current_frame_timestamp_ != packet->packet.timestamp)
        {
            // A timestamp boundary without a marker abandons the previous AU.
            DropCurrentFrame(seq - 1);
        }
        if (!current_frame_start_)
        {
            current_frame_start_ = seq;
            current_frame_timestamp_ = packet->packet.timestamp;
        }

        const bool marker = packet->packet.marker;
        if (static_cast<uint64_t>(seq - *current_frame_start_ + 1) > config_.max_frame_packets)
        {
            if (!discarding_frame_)
                RemovePackets(*current_frame_start_, seq);
            else
                RemovePackets(seq, seq);
            // Retain the AU identity until its marker/timestamp boundary, so
            // packets after the limit cannot become a separate complete suffix.
            discarding_frame_ = true;
        }
        if (!marker) continue;

        if (!discarding_frame_)
        {
            H264AccessUnit frame;
            if (MaybeAssembleFrame(*current_frame_start_, seq, frame))
                result.frames.emplace_back(std::move(frame));
        }
        DropCurrentFrame(seq);
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

    std::optional<uint8_t> validating_fu;
    bool saw_vcl = false;
    size_t unit_count = 0;
    size_t payload_bytes = 0;
    for (int64_t seq = start_seq; seq <= end_seq; ++seq)
    {
        auto* buffered = GetPacket(seq);
        if (!buffered || buffered->packet.ssrc != frame.ssrc ||
            buffered->packet.timestamp != frame.timestamp)
            return false;
        frame.receive_time_ms = std::max(frame.receive_time_ms,
                                         buffered->packet.receive_time_ms);
        frame.keyframe = frame.keyframe || buffered->packet.has_key_nalu;
        frame.has_sps = frame.has_sps || buffered->packet.has_sps;
        frame.has_pps = frame.has_pps || buffered->packet.has_pps;
        frame.has_idr = frame.has_idr || buffered->packet.has_idr;
        unit_count += buffered->packet.units.size();
        for (const auto& unit : buffered->packet.units)
        {
            payload_bytes += unit.data.size();
            if (!saw_vcl && (unit.nal_type == 1 || unit.nal_type == 5))
            {
                // Prefix NALs do not prove that the first picture slice arrived.
                if (!unit.first_slice) return false;
                saw_vcl = true;
            }
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
    common::ByteVector fu_nalu;
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
    if (end_seq - start_seq >= static_cast<int64_t>(kBufferSize))
    {
        for (auto& packet : buffer_)
        {
            if (packet && packet->unwrapped_seq >= start_seq && packet->unwrapped_seq <= end_seq)
            {
                ErasePacket(packet);
            }
        }
        return;
    }
    for (int64_t seq = start_seq; seq <= end_seq; ++seq)
    {
        auto& packet = buffer_[BufferIndex(seq)];
        if (packet && packet->unwrapped_seq == seq)
        {
            ErasePacket(packet);
        }
    }
}

void H264PacketBuffer::RemovePacketsBefore(int64_t seq)
{
    for (auto& packet : buffer_)
    {
        if (packet && packet->unwrapped_seq < seq)
        {
            ErasePacket(packet);
        }
    }
}

void H264PacketBuffer::DropCurrentFrame(int64_t end_seq)
{
    if (current_frame_start_) RemovePackets(*current_frame_start_, end_seq);
    current_frame_start_.reset();
    discarding_frame_ = false;
}

bool H264PacketBuffer::ValidatePacketUnits(const H264ParsedPacket& packet,
                                           std::optional<uint8_t>& fu_header) const
{
    for (const auto& unit : packet.units)
    {
        if (unit.unit_type == H264PayloadUnitType::SingleNalu ||
            unit.unit_type == H264PayloadUnitType::StapANalu)
        {
            if (fu_header || unit.data.empty())
                return false;
            continue;
        }
        if (unit.unit_type != H264PayloadUnitType::FuAFragment || unit.data.empty())
            return false;
        if (unit.fu_start)
        {
            if (fu_header)
                return false;
            fu_header = unit.reconstructed_nal_header;
        }
        else if (!fu_header)
            return false;
        if (*fu_header != unit.reconstructed_nal_header)
            return false;
        if (unit.fu_end)
            fu_header.reset();
    }
    return true;
}

bool H264PacketBuffer::AppendPacketUnits(H264ParsedPacket& packet, bool& fu_active,
                                         common::ByteVector& fu_nalu,
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
        if (fu_nalu.front() != unit.reconstructed_nal_header)
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
