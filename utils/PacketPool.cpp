#include "PacketPool.h"


PacketPool::PacketPool(std::size_t packet_count)
{
    nodes_.resize(packet_count);
    packets_.resize(packet_count);

    for (size_t i = 0; i < packet_count; i++)
    {
        packets_[i].owner = this;

        nodes_[i].pkt = &packets_[i];
        nodes_[i].next = (i + 1 < packet_count) ? &nodes_[i + 1] : nullptr;
    }

    head_.store(&nodes_[0], std::memory_order_release);
}

PacketPool::~PacketPool()
{

}

Packet* PacketPool::acquire()
{
   Node* old_head = head_.load(std::memory_order_acquire);
   while (old_head)
   {
        Node* next = old_head->next;
        if (head_.compare_exchange_weak(
                    old_head, next,
                    std::memory_order_acquire,
                    std::memory_order_relaxed))
        {
            return old_head->pkt;
        }
   }
   return nullptr;
}

void PacketPool::release(Packet* pkt)
{
    if (!pkt) return;
    pkt->reset();

    Node* node = node_from_packet(pkt);

    Node* old_head = head_.load(std::memory_order_relaxed);

    do
    {
        node->next = old_head;
    } while (!head_.compare_exchange_weak(
        old_head, node,
        std::memory_order_release,
        std::memory_order_relaxed));

}
