#ifndef _PACKET_H_
#define _PACKET_H_

#include <vector>
#include <mutex>
#include <cstdint>

struct Packet
{
    uint8_t*  data;
    uint16_t  len;
    uint16_t  cap;

    uint8_t   flags;        // RTP / RTCP / retrans / keyframe?
    uint64_t  recv_ts;      // socket 收到时间
    uint64_t  enqueue_ts;   // 入 ring 时间
};

class PacketPool
{
public:
    PacketPool(std::size_t packet_capacity,
               std::size_t packet_count);

    ~PacketPool();

    // 获取一个 Packet；失败返回 nullptr（上层直接丢包）
    Packet* acquire();

    // 归还 Packet（必须来自 acquire）
    void release(Packet* pkt);

    // 观测
    std::size_t capacity() const { return capacity_; }
    std::size_t size() const;

    struct Stats
    {
        uint64_t acquired = 0;
        uint64_t released = 0;
        uint64_t exhausted = 0;
    };
    Stats stats() const;

private:
    std::size_t capacity_;
    mutable std::mutex mtx_;
    std::vector<Packet*> free_;
    Stats stats_{};
};


#endif /* _PACKET_H_ */