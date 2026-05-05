#include "RtpPacketHistory.h"

RtpPacketHistory::RtpPacketHistory(const Config& config)
    : config_(config) 
{
}

void RtpPacketHistory::Put(uint16_t sequence_number, const uint8_t* data, size_t size, uint64_t now_ms)
{
    if (data == nullptr || size == 0) 
    {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    RemoveExpiredLocked(now_ms);

    auto iter = packets_.find(sequence_number);
    if (iter == packets_.end()) 
    {
        packet_order_.push_back(sequence_number);
    }

    StoredPacket packet;
    packet.sequence_number = sequence_number;
    packet.data.assign(data, data + size);
    packet.stored_time_ms = now_ms;
    packet.last_retransmit_time_ms = 0;
    packet.retransmit_count = 0;

    packets_[sequence_number] = std::move(packet);

    TrimToSizeLocked();

}

bool RtpPacketHistory::PutRtpPacket(const uint8_t* data, size_t size, uint64_t now_ms)
{
    uint16_t sequence_number = 0;
    if (!ParseRtpSequenceNumber(data, size, sequence_number)) 
    {
        return false;
    }

    Put(sequence_number, data, size, now_ms);
    return true;
}

std::optional<RtpPacketHistory::StoredPacket>
RtpPacketHistory::Get(uint16_t sequence_number) const 
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto iter = packets_.find(sequence_number);
    if (iter == packets_.end()) 
    {
        return std::nullopt;
    }

    return iter->second;
}

void RtpPacketHistory::MarkRetransmitted(uint16_t sequence_number, uint64_t now_ms) 
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto iter = packets_.find(sequence_number);
    if (iter == packets_.end()) 
    {
        return;
    }

    iter->second.last_retransmit_time_ms = now_ms;
    iter->second.retransmit_count++;
}

void RtpPacketHistory::RemoveExpired(uint64_t now_ms) 
{
    std::lock_guard<std::mutex> lock(mutex_);
    RemoveExpiredLocked(now_ms);
}


void RtpPacketHistory::Clear() 
{
    std::lock_guard<std::mutex> lock(mutex_);
    packets_.clear();
    packet_order_.clear();
}

size_t RtpPacketHistory::Size() const 
{
    std::lock_guard<std::mutex> lock(mutex_);
    return packets_.size();
}

bool RtpPacketHistory::ParseRtpSequenceNumber(const uint8_t* data, size_t size, uint16_t& sequence_number) 
{
    if (data == nullptr || size < 12) 
    {
        return false;
    }


    const uint8_t version = data[0] >> 6;
    if (version != 2) 
    {
        return false;
    }

    sequence_number = static_cast<uint16_t>((static_cast<uint16_t>(data[2]) << 8) | static_cast<uint16_t>(data[3]));

    return true;
}

void RtpPacketHistory::TrimToSizeLocked()
{
    while (packets_.size() > config_.max_packets && !packet_order_.empty())
    {
        uint16_t old_seq = packet_order_.front();
        packet_order_.pop_front();
        packets_.erase(old_seq);
    }
}

void RtpPacketHistory::RemoveExpiredLocked(uint64_t now_ms)
{
    while (!packet_order_.empty())
    {
        uint16_t seq = packet_order_.front();

        auto iter = packets_.find(seq);
        if (iter == packets_.end())
        {
            packet_order_.pop_front();
            continue;
        }

        const uint64_t stored_time_ms = iter->second.stored_time_ms;

        if (now_ms < stored_time_ms)
        {
            break;
        }

        const uint64_t age_ms = now_ms - stored_time_ms;
        if (age_ms <= config_.max_packet_age_ms) 
        {
            break;
        }

        packets_.erase(iter);
        packet_order_.pop_front();
    }
}