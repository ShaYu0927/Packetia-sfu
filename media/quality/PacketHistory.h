#ifndef _PACKET_HISTORY_H_
#define _PACKET_HISTORY_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include "NetworkStats.h"

namespace media
{

class PacketHistory
{
public:
    explicit PacketHistory(size_t max_packets = 2048);

    void AddPacket(const PacketSendInfo& packet);
    bool GetPacket(int64_t transport_sequence, PacketSendInfo& packet) const;
    // Resolve the nearest cycle relative to the latest successful send. The
    // active feedback window must span fewer than 32768 transport sequences.
    bool GetPacketByWireSequence(uint16_t sequence, PacketSendInfo& packet) const;
    void RemovePacket(int64_t transport_sequence);
    void Clear();

private:
    size_t max_packets_ = 2048;
    int64_t latest_sequence_ = -1;
    std::deque<int64_t> order_;
    std::unordered_map<int64_t, PacketSendInfo> packets_;
};

}

#endif /* _PACKET_HISTORY_H_ */
