#ifndef PACKETIA_MEDIA_QUALITY_TRANSPORT_SEQUENCE_ALLOCATOR_H_
#define PACKETIA_MEDIA_QUALITY_TRANSPORT_SEQUENCE_ALLOCATOR_H_

#include <atomic>
#include <cstdint>

namespace media
{

/**
 * 一次 Transport-CC 序号分配结果。
 *
 * wire_sequence:
 *   写入 RTP Header Extension 的 16 位序号，会在 65535 后回绕到 0。
 *
 * extended_sequence:
 *   仅在本地使用的连续序号，用作 PacketHistory 的键。它不会随 16 位
 *   序号回绕，可以区分相隔 65536 个包但 wire_sequence 相同的两个包。
 */
struct TransportSequenceNumber
{
    uint16_t wire_sequence = 0;
    int64_t extended_sequence = 0;
};

/**
 * 每条下行 Transport 共享的 TWCC 序号分配器。
 *
 * 所有走同一网络路径并共享拥塞控制器的音频、视频、RTX、FEC 和
 * Padding 发送器都必须持有同一个实例。不同订阅者或不同 ICE Transport
 * 必须使用不同实例，不能在服务器进程内设置一个全局分配器。
 *
 * Allocate() 是线程安全的，允许音频和视频在不同工作线程中并发取号。
 * 分配只代表为一个即将发送的包预留序号；调用方仍需在实际发送成功时
 * 将发送时间、包大小和 extended_sequence 写入 PacketHistory。
 */
class TransportSequenceAllocator
{
public:
    explicit TransportSequenceAllocator(uint16_t initial_sequence = 0) noexcept
        : next_extended_sequence_(initial_sequence)
    {
    }

    // 原子分配下一个序号，同时返回线上 16 位值和本地扩展值。
    TransportSequenceNumber Allocate() noexcept
    {
        // relaxed 足够保证每次 fetch_add 得到唯一值；这里不负责同步 RTP 包内容。
        const int64_t extended = next_extended_sequence_.fetch_add(1, std::memory_order_relaxed);
        return {static_cast<uint16_t>(static_cast<uint64_t>(extended) & 0xFFFFU), extended};
    }

    // 查看下一次将分配的扩展序号，不消耗序号，主要用于监控和测试。
    int64_t PeekNextExtendedSequence() const noexcept
    {
        return next_extended_sequence_.load(std::memory_order_relaxed);
    }

    /**
     * 在 Transport 路由重建时重置序号空间。
     *
     * Reset 与 Allocate 虽然不会产生数据竞争，但路由切换方仍应先停止
     * 旧路径发送，并同时清空 PacketHistory 和反馈解包器的回绕状态。
     */
    void Reset(uint16_t initial_sequence = 0) noexcept
    {
        next_extended_sequence_.store(initial_sequence, std::memory_order_relaxed);
    }

private:
    std::atomic<int64_t> next_extended_sequence_{0};
};

}

#endif /* PACKETIA_MEDIA_QUALITY_TRANSPORT_SEQUENCE_ALLOCATOR_H_ */
