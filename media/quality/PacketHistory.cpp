#include "PacketHistory.h"

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

    while (packets_.size() > max_packets_ && !order_.empty())
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

void PacketHistory::Clear()
{
    order_.clear();
    packets_.clear();
}

}