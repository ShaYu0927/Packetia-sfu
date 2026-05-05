#ifndef _RTP_PACKET_HISTORY_H_
#define _RTP_PACKET_HISTORY_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <optional>

class RtpPacketHistory
{
public:
    struct Config 
    {
        size_t   max_packets = 1024;
        uint64_t max_packet_age_ms = 3000;
    };

    struct StoredPacket
    {
        /**
         * @brief RTP sequence number。
         */
        uint16_t sequence_number = 0;

        /**
         * @brief RTP 原始包数据
         *
         * 包含 RTP header + RTP payload
         */
        std::vector<uint8_t> data;

        /**
         * @brief 第一次缓存时间，单位毫秒。
         */
        uint64_t stored_time_ms = 0;

        /**
         * @brief 最近一次重传时间，单位毫秒。
         */
        uint64_t last_retransmit_time_ms = 0;

        /**
         * @brief 重传次数。
         */
        uint32_t retransmit_count = 0;
    };

public:
    explicit RtpPacketHistory(const Config& config);
    ~RtpPacketHistory() = default;

    RtpPacketHistory(const RtpPacketHistory&) = delete;
    RtpPacketHistory& operator=(const RtpPacketHistory&) = delete;

public:
    /**
     * @brief 缓存一个 RTP 包。
     *
     * @param sequence_number RTP seq。
     * @param data RTP 包数据起始地址。
     * @param size RTP 包大小。
     * @param now_ms 当前时间，单位毫秒。
     */
    void Put(uint16_t sequence_number, const uint8_t* data, size_t size, uint64_t now_ms);

     /**
     * @brief 从 RTP 原始包里解析 seq 并缓存
     *
     * RTP header 中 sequence number 位于第 2、3 字节。
     */
    bool PutRtpPacket(const uint8_t* data, size_t size, uint64_t now_ms);

     /**
     * @brief 根据 RTP seq 查找历史包
     *
     * @return 找到则返回 StoredPacket 副本，找不到返回 std::nullopt。
     */
    std::optional<StoredPacket> Get(uint16_t sequence_number) const;

    /**
     * @brief 标记某个包被重传了一次
     *
     * 收到 NACK 并完成重传后调用，方便后续做限频或统计
     */
    void MarkRetransmitted(uint16_t sequence_number, uint64_t now_ms);


     /**
     * @brief 清理过期 RTP 包。
     */
    void RemoveExpired(uint64_t now_ms);


    /**
     * @brief 清空历史缓存。
     */
    void Clear();

    /**
     * @brief 当前缓存包数量。
     */
    size_t Size() const;

private:
    static bool ParseRtpSequenceNumber(const uint8_t* data, size_t size, uint16_t& sequence_number);

    void TrimToSizeLocked();
    void RemoveExpiredLocked(uint64_t now_ms);

private:
    Config config_;

    mutable std::mutex mutex_;

    /**
     * @brief seq -> RTP 包。
     */
    std::unordered_map<uint16_t, StoredPacket> packets_;

    /**
     * @brief 按插入顺序记录 seq，用于淘汰旧包。
     */
    std::deque<uint16_t> packet_order_;
};


#endif /* _RTP_PACKET_HISTORY_H_ */