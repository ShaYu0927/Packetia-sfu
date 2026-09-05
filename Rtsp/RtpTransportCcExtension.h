#ifndef PACKETIA_RTP_TRANSPORT_CC_EXTENSION_H_
#define PACKETIA_RTP_TRANSPORT_CC_EXTENSION_H_

#include <cstddef>
#include <cstdint>

namespace rtsp
{

/**
 * 读取 RTP 中的 Transport-Wide CC 序号。
 *
 * extension_id 必须来自当前 m= section 的 SDP a=extmap 协商结果。
 * 当前只接受 RFC 8285 one-byte 格式（profile=0xBEDE，ID 1~14）。
 */
class RtpTransportCcExtension
{
public:
    static bool Read(const uint8_t* packet,
                     size_t size,
                     uint8_t extension_id,
                     uint16_t* transport_sequence);
};

}

#endif /* PACKETIA_RTP_TRANSPORT_CC_EXTENSION_H_ */
