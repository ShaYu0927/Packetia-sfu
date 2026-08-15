#ifndef _RTSP_INTERLEAVED_TRANSPORT_H_
#define _RTSP_INTERLEAVED_TRANSPORT_H_

#include "MediaTransportBase.h"
#include "TcpConnection.h"

#include <cstdint>
#include <memory>

namespace media::transport
{

class RtspInterleavedTransport final : public MediaTransportBase
{
public:
    RtspInterleavedTransport(uint64_t id,
                             std::weak_ptr<TcpConnection> connection,
                             uint8_t rtp_channel,
                             uint8_t rtcp_channel);

    MediaTransportProtocol Protocol() const noexcept override;

    SendResult Send(MediaPacketType type, const uint8_t* data, size_t size, bool retransmit = false) override;

    void Close() override;

    MediaPacketIngressResult InputInterleaved(uint8_t channel, const uint8_t* data, size_t size, uint64_t receive_time_ms);

private:
    std::weak_ptr<TcpConnection> connection_;
    const uint8_t rtp_channel_;
    const uint8_t rtcp_channel_;
};

} // namespace media::transport

#endif /* _RTSP_INTERLEAVED_TRANSPORT_H_ */
