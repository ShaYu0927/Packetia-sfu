#ifndef _MEDIA_TRANSPORT_PACKET_H_
#define _MEDIA_TRANSPORT_PACKET_H_

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>
#include "../../Common/memory/SharedBuffer.h"

enum class MediaPacketType
{
    Rtp = 0,
    Rtcp
};

/*
 * One complete RTP or RTCP packet received from a media transport.
 *
 * The payload owns its storage so the packet can safely move from an I/O
 * thread to a media worker. Protocol-specific framing (for example the RTSP
 * interleaved '$' header) is not part of the payload.
 */
class ReceivedMediaPacket
{
public:
    static constexpr int kNoChannel = -1;

    ReceivedMediaPacket() = default;

    ReceivedMediaPacket(MediaPacketType packet_type,
                        uint64_t source_transport_id,
                        uint64_t received_at_ms,
                        const uint8_t* data,
                        size_t size,
                        int source_channel = kNoChannel)
        : type(packet_type),
          transport_id(source_transport_id),
          receive_time_ms(received_at_ms),
          channel(source_channel)
    {
        if (data && size > 0)
        {
            payload = common::SharedBuffer::TryCopy(data, size);
        }
    }

    ReceivedMediaPacket(MediaPacketType packet_type,
                        uint64_t source_transport_id,
                        uint64_t received_at_ms,
                        std::vector<uint8_t> owned_payload,
                        int source_channel = kNoChannel)
        : type(packet_type),
          transport_id(source_transport_id),
          receive_time_ms(received_at_ms),
          channel(source_channel),
          payload(common::SharedBuffer::TryCopy(owned_payload.data(), owned_payload.size()))
    {
    }

    ReceivedMediaPacket(MediaPacketType packet_type,
                        uint64_t source_transport_id,
                        uint64_t received_at_ms,
                        common::SharedBuffer owned_payload,
                        int source_channel = kNoChannel)
        : type(packet_type), transport_id(source_transport_id),
          receive_time_ms(received_at_ms), channel(source_channel),
          payload(std::move(owned_payload)) {}

    bool IsValid() const noexcept
    {
        return transport_id != 0 && !payload.Empty();
    }

    const uint8_t* Data() const noexcept { return payload.Data(); }
    size_t Size() const noexcept { return payload.Size(); }

public:
    MediaPacketType type = MediaPacketType::Rtp;
    uint64_t transport_id = 0;
    uint64_t receive_time_ms = 0;
    int channel = kNoChannel;
    common::SharedBuffer payload;
};

#endif /* _MEDIA_TRANSPORT_PACKET_H_ */
