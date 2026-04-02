#ifndef _PACKET_H_
#define _PACKET_H_

#include <atomic>
#include <cstring>
#include <vector>
#include <cstdint>

class PacketPool;

constexpr size_t MAX_PACKET_SIZE = 1500;

enum PacketFlags : uint8_t
{
    PKT_F_NONE  = 0,
    PKT_F_RTP   = 1 << 0,
    PKT_F_RTCP  = 1 << 1,
    PKT_F_STUN  = 1 << 2,
    PKT_F_DTLS  = 1 << 3,
};


struct Packet
{
    PacketPool* owner = nullptr;
    uint16_t len = 0;
    uint16_t cap = MAX_PACKET_SIZE;

    uint8_t     flags = 0;
    uint64_t    recv_ts = 0;
    uint64_t    enqueue_ts = 0;

    uint8_t data[MAX_PACKET_SIZE];

    Packet() = default;

    Packet(const uint8_t* src, size_t n)
    {
        assign(src, n);
    }

    void assign(const uint8_t* src, size_t n)
    {
        len = std::min((size_t)MAX_PACKET_SIZE, n);
        memcpy(data, src, len);
    }

    inline void reset()
    {
        len = 0;
        flags = 0;
        recv_ts = 0;
        enqueue_ts = 0;
    }
};


class PacketPool
{
public:
    static PacketPool& instance()
    {
        thread_local PacketPool inst(2048);
        return inst;
    }

    ~PacketPool();

    Packet* acquire();

    void release(Packet* pkt);
    std::size_t size() const;

private:
    PacketPool(std::size_t packet_count);

    struct Node
    {
        Packet* pkt;
        Node* next;
    };

    Node* node_from_packet(Packet* pkt)
    {
        size_t idx = pkt - packets_.data();
        return &nodes_[idx];
    }

    std::vector<Packet> packets_;
    std::vector<Node> nodes_;

    std::atomic<Node*> head_{nullptr};
};


#endif /* _PACKET_H_ */