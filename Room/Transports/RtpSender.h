#ifndef _RTP_SENDER_H_
#define _RTP_SENDER_H_

#include "Transports/RtpPacketHistory.h"
#include <cstdint>
#include <memory>
namespace room 
{
class ITransport
{
public:
    virtual ~ITransport() = default;

    /**
     * @brief 发送 RTP/RTCP 数据
     *
     * 初期可以是 UDP SendTo
     * 后面 WebRTC 场景可以替换成 SRTP/DTLS Transport
     */
    virtual bool Send(const uint8_t* data, std::size_t size) = 0;
};

class RtpSender
{
public:
    explicit RtpSender(std::shared_ptr<ITransport> transport);
    ~RtpSender() = default;

    RtpSender(const RtpSender&) = delete;
    RtpSender& operator=(const RtpSender&) = delete;

public:
    /**
     * @brief 发送 RTP 包
     *
     * @param data RTP packet buffer，包含 RTP header + payload
     * @param size RTP packet size
     * @param now_ms 当前时间，单位毫秒
     */
    bool SendRtpPacket(const uint8_t* data, size_t size, uint64_t now_ms);

    /**
     * @brief 收到 NACK 后重传指定 seq 的 RTP 包
     */
    void Retransmit(uint16_t sequence_number, uint64_t now_ms);

    /**
     * @brief 批量处理 NACK
     */
    void OnNack(const std::vector<uint16_t>& lost_sequences, uint64_t now_ms);

    /**
     * @brief 获取 RTP 历史缓存
     */
    RtpPacketHistory& PacketHistory();

private:
    std::shared_ptr<ITransport> transport_;
    RtpPacketHistory packet_history_;
};

}

#endif /* _RTP_SENDER_H_ */