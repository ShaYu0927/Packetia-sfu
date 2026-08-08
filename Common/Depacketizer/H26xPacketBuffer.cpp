#include "H26xPacketBuffer.h"

#include <algorithm>
#include <chrono>

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

H26xPacketBuffer::H26xPacketBuffer()
    : H26xPacketBuffer(Config{})
{
}

H26xPacketBuffer::H26xPacketBuffer(const Config& config)
    : config_(config)
{
    if (config_.max_frame_packets == 0 || config_.max_frame_packets > kBufferSize)
        config_.max_frame_packets = kBufferSize;
}

void H26xPacketBuffer::Reset()
{
    for (auto& slot : buffer_)
        slot.Reset();
    seq_unwrapper_.Reset();
    has_received_packet_ = false;
    has_last_output_seq_ = false;
    last_output_seq_ = -1;
    buffered_packet_count_ = 0;
}

H26xPacketBuffer::InsertResult H26xPacketBuffer::InsertPacket(H264ParsedPacket packet, int64_t now_ms)
{
    InsertResult result;
    if (!packet.valid || packet.malformed)
        return result;
    if (now_ms < 0)
        now_ms = GetNowMs();

    const int64_t unwrapped_seq = seq_unwrapper_.Unwrap(packet.seq);
    if (IsPacketStale(unwrapped_seq))
    {
        result.late = true;
        return result;
    }

    auto& slot = buffer_[BufferIndex(unwrapped_seq)];
    if (slot.occupied)
    {
        if (slot.unwrapped_seq == unwrapped_seq)
        {
            result.duplicate = true;
            return result;
        }
        result.buffer_collision = true;
        RecoverFromBufferCollision(unwrapped_seq);
        result.buffer_reset = true;
    }

    auto& target = buffer_[BufferIndex(unwrapped_seq)];
    target.occupied = true;
    target.unwrapped_seq = unwrapped_seq;
    target.arrival_ms = now_ms;
    target.packet = std::move(packet);
    ++buffered_packet_count_;
    has_received_packet_ = true;
    result.inserted = true;

    FindFrames(unwrapped_seq, result);
    DropExpiredFrames(now_ms, result);
    return result;
}

size_t H26xPacketBuffer::BufferedPacketCount() const
{
    return buffered_packet_count_;
}

int64_t H26xPacketBuffer::GetNowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

size_t H26xPacketBuffer::BufferIndex(int64_t unwrapped_seq)
{
    return static_cast<size_t>(unwrapped_seq) & (kBufferSize - 1);
}

H26xPacketBuffer::BufferedPacket* H26xPacketBuffer::GetPacket(int64_t seq)
{
    auto& slot = buffer_[BufferIndex(seq)];
    return slot.occupied && slot.unwrapped_seq == seq ? &slot : nullptr;
}

const H26xPacketBuffer::BufferedPacket* H26xPacketBuffer::GetPacket(int64_t seq) const
{
    const auto& slot = buffer_[BufferIndex(seq)];
    return slot.occupied && slot.unwrapped_seq == seq ? &slot : nullptr;
}

bool H26xPacketBuffer::IsPacketStale(int64_t seq) const
{
    return has_last_output_seq_ && seq <= last_output_seq_;
}

bool H26xPacketBuffer::IsFrameEnd(const H264ParsedPacket& packet) const
{
    return packet.marker || (config_.allow_ends_frame && packet.ends_frame);
}

bool H26xPacketBuffer::BeginningOfStream(int64_t seq, const H264ParsedPacket& packet) const
{
    if (packet.begins_frame)
        return true;
    if (has_last_output_seq_)
        return seq == last_output_seq_ + 1;
    return !GetPacket(seq - 1);
}

void H26xPacketBuffer::CollectCandidateFrameEnds(int64_t inserted_seq,
                                                  std::vector<int64_t>& candidates) const
{
    const auto* inserted = GetPacket(inserted_seq);
    if (inserted && IsFrameEnd(inserted->packet))
        candidates.push_back(inserted_seq);

    // A missing packet may arrive after the marker packet, so retry every buffered end.
    for (const auto& slot : buffer_)
    {
        if (slot.occupied && slot.unwrapped_seq != inserted_seq && IsFrameEnd(slot.packet))
            candidates.push_back(slot.unwrapped_seq);
    }
    std::sort(candidates.begin(), candidates.end());
}

void H26xPacketBuffer::FindFrames(int64_t inserted_seq, InsertResult& result)
{
    std::vector<int64_t> candidates;
    CollectCandidateFrameEnds(inserted_seq, candidates);
    for (int64_t end_seq : candidates)
    {
        if (!GetPacket(end_seq) || IsPacketStale(end_seq))
            continue;
        H264AccessUnit frame;
        int64_t start_seq = -1;
        if (MaybeAssembleFrame(end_seq, frame, start_seq))
        {
            ClearPackets(start_seq, end_seq);
            last_output_seq_ = end_seq;
            has_last_output_seq_ = true;
            result.frames.emplace_back(std::move(frame));
        }
    }
}

bool H26xPacketBuffer::FindFrameStart(int64_t end_seq, int64_t& start_seq) const
{
    const auto* end = GetPacket(end_seq);
    if (!end)
        return false;

    start_seq = end_seq;
    for (size_t count = 1; count < config_.max_frame_packets; ++count)
    {
        const auto* current = GetPacket(start_seq);
        const auto* previous = GetPacket(start_seq - 1);
        if (!previous)
            return BeginningOfStream(start_seq, current->packet);
        if (previous->packet.ssrc != end->packet.ssrc ||
            previous->packet.timestamp != end->packet.timestamp ||
            IsFrameEnd(previous->packet))
            return true;
        --start_seq;
    }
    return false;
}

bool H26xPacketBuffer::MaybeAssembleFrame(int64_t end_seq, H264AccessUnit& frame,
                                           int64_t& start_seq)
{
    if (!FindFrameStart(end_seq, start_seq))
        return false;
    if (!AssembleFrame(start_seq, end_seq, frame))
        return false;
    frame.first_seq = static_cast<uint16_t>(start_seq);
    frame.last_seq = static_cast<uint16_t>(end_seq);
    frame.complete = true;
    frame.marker = GetPacket(end_seq)->packet.marker;
    return true;
}

bool H26xPacketBuffer::AssembleFrame(int64_t start_seq, int64_t end_seq, H264AccessUnit& frame) const
{
    const auto* first = GetPacket(start_seq);
    if (!first)
        return false;
    frame.ssrc = first->packet.ssrc;
    frame.timestamp = first->packet.timestamp;
    bool fu_active = false;
    std::vector<uint8_t> fu_nalu;

    for (int64_t seq = start_seq; seq <= end_seq; ++seq)
    {
        const auto* buffered = GetPacket(seq);
        if (!buffered || buffered->packet.ssrc != frame.ssrc ||
            buffered->packet.timestamp != frame.timestamp)
            return false;
        if (!AppendPacketUnits(buffered->packet, fu_active, fu_nalu, frame))
            return false;
        frame.keyframe = frame.keyframe || buffered->packet.has_key_nalu;
        frame.has_sps = frame.has_sps || buffered->packet.has_sps;
        frame.has_pps = frame.has_pps || buffered->packet.has_pps;
        frame.has_idr = frame.has_idr || buffered->packet.has_idr;
    }
    return !fu_active && !frame.nalus.empty();
}

bool H26xPacketBuffer::AppendPacketUnits(const H264ParsedPacket& packet,
                                         bool& fu_active,
                                         std::vector<uint8_t>& fu_nalu,
                                         H264AccessUnit& frame) const
{
    for (const auto& unit : packet.units)
    {
        if (unit.unit_type == H264PayloadUnitType::SingleNalu ||
            unit.unit_type == H264PayloadUnitType::StapANalu)
        {
            if (fu_active || unit.data.empty())
                return false;
            frame.nalus.push_back(unit.data);
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

void H26xPacketBuffer::ClearPackets(int64_t start_seq, int64_t end_seq)
{
    for (int64_t seq = start_seq; seq <= end_seq; ++seq)
    {
        auto* packet = GetPacket(seq);
        if (packet)
        {
            packet->Reset();
            --buffered_packet_count_;
        }
    }
}

void H26xPacketBuffer::ClearFrameByTimestamp(uint32_t ssrc, uint32_t timestamp)
{
    for (auto& slot : buffer_)
    {
        if (slot.occupied && slot.packet.ssrc == ssrc && slot.packet.timestamp == timestamp)
        {
            slot.Reset();
            --buffered_packet_count_;
        }
    }
}

void H26xPacketBuffer::DropExpiredFrames(int64_t now_ms, InsertResult& result)
{
    if (config_.max_frame_delay_ms < 0)
        return;
    struct Expired { uint32_t ssrc; uint32_t timestamp; };
    std::vector<Expired> expired;
    for (const auto& slot : buffer_)
    {
        if (!slot.occupied || now_ms - slot.arrival_ms <= config_.max_frame_delay_ms)
            continue;
        const auto same = [&](const Expired& item) {
            return item.ssrc == slot.packet.ssrc && item.timestamp == slot.packet.timestamp;
        };
        if (std::find_if(expired.begin(), expired.end(), same) == expired.end())
            expired.push_back({slot.packet.ssrc, slot.packet.timestamp});
    }
    for (const auto& item : expired)
    {
        ClearFrameByTimestamp(item.ssrc, item.timestamp);
        ++result.dropped_frames;
    }
}

void H26xPacketBuffer::RecoverFromBufferCollision(int64_t new_unwrapped_seq)
{
    (void)new_unwrapped_seq;
    for (auto& slot : buffer_)
        slot.Reset();
    buffered_packet_count_ = 0;
}

} // namespace media
