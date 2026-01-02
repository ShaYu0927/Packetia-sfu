#include "PacketPool.h"


PacketPool::PacketPool(std::size_t packet_capacity,
                       std::size_t packet_count)
    : capacity_(packet_capacity)
{
    free_.reserve(packet_count);
    for (std::size_t i = 0; i < packet_count; ++i)
    {
        auto* pkt = new Packet;
        pkt->data = new uint8_t[packet_capacity];
        pkt->cap  = static_cast<uint16_t>(packet_capacity);
        pkt->len  = 0;
        pkt->enqueue_ts = 0;
        free_.push_back(pkt);
    }
}

PacketPool::~PacketPool()
{
    for (auto* pkt : free_)
    {
        delete[] pkt->data;
        delete pkt;
    }
}

Packet* PacketPool::acquire()
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (free_.empty())
    {
        stats_.exhausted++;
        return nullptr;
    }

    Packet* pkt = free_.back();
    free_.pop_back();
    pkt->len = 0;
    stats_.acquired++;
    return pkt;
}

void PacketPool::release(Packet* pkt)
{
    if (!pkt) return;

    std::lock_guard<std::mutex> lk(mtx_);
    free_.push_back(pkt);
    stats_.released++;
}

std::size_t PacketPool::size() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    return free_.size();
}

PacketPool::Stats PacketPool::stats() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    return stats_;
}