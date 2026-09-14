#include "PacketHistory.h"
#include <algorithm>

namespace media
{

PacketHistory::PacketHistory(size_t max_packets)
    : max_packets_(max_packets == 0 ? 1 : max_packets)
{
}

void PacketHistory::AddPacket(const PacketSendInfo& packet)
{
    if (packet.transport_sequence < 0)
    {
        return;
    }

    const int64_t key = packet.transport_sequence;
    if (packets_.find(key) == packets_.end())
    {
        order_.push_back(key);
    }

    packets_[key] = packet;
    latest_sequence_ = std::max(latest_sequence_, key);

    while (order_.size() > max_packets_)
    {
        const int64_t old_key = order_.front();
        order_.pop_front();
        packets_.erase(old_key);
    }
}

bool PacketHistory::GetPacket(int64_t transport_sequence, PacketSendInfo& packet) const
{
    auto it = packets_.find(transport_sequence);
    if (it == packets_.end())
    {
        return false;
    }

    packet = it->second;
    return true;
}

bool PacketHistory::GetPacketByWireSequence(uint16_t sequence, PacketSendInfo& packet) const
{
    if (latest_sequence_ < 0) return false;
    int32_t delta = static_cast<int32_t>(sequence) -
                    static_cast<int32_t>(static_cast<uint16_t>(latest_sequence_));
    if (delta > 32767) delta -= 65536;
    if (delta < -32768) delta += 65536;
    return GetPacket(latest_sequence_ + delta, packet);
}

void PacketHistory::RemovePacket(int64_t transport_sequence)
{
    packets_.erase(transport_sequence);
}

void PacketHistory::Clear()
{
    latest_sequence_ = -1;
    order_.clear();
    packets_.clear();
}

}
