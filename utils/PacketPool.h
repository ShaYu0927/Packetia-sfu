#ifndef _PACKET_H_
#define _PACKET_H_

#include <vector>
#include <mutex>
#include <cstdint>


class PacketPool;
struct Packet
{
    PacketPool* owner = nullptr;   
    uint16_t    len   = 0;
    uint16_t    cap   = 0;

    uint8_t     flags = 0;
    uint64_t    recv_ts = 0;
    uint64_t    enqueue_ts = 0;

    uint8_t*    data = nullptr;    // 指向 storage.data()
    std::vector<uint8_t> storage;  // 固定大小，不在每次 acquire/release 分配

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
    PacketPool(std::size_t packet_capacity,
               std::size_t packet_count);

    ~PacketPool();

    Packet* acquire();

    void release(Packet* pkt);

    // 观测
    std::size_t capacity() const { return capacity_; }
    std::size_t size() const;

    struct Stats
    {
        uint64_t acquired = 0;
        uint64_t released = 0;
        uint64_t exhausted = 0;
        uint64_t bad_release = 0;
    };
    Stats stats() const;

private:
    std::size_t capacity_;
    mutable std::mutex mtx_;
    std::vector<Packet*> free_;
    Stats stats_{};
};


#endif /* _PACKET_H_ */