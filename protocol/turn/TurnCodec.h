#ifndef PACKETIA_TURN_CODEC_H
#define PACKETIA_TURN_CODEC_H

#include "Stun.h"
#include "TurnConstants.h"

namespace protocol
{

struct TurnChannelDataView
{
    uint16_t channel = 0;
    // Borrows the input datagram; valid only while that buffer is alive.
    std::string_view data;
};

class TurnCodec
{
public:
    // Preserve the existing typed API; values have one macro definition.
    static constexpr uint16_t kFirstChannel = PACKETIA_TURN_FIRST_CHANNEL;
    static constexpr uint16_t kLastChannel = PACKETIA_TURN_LAST_CHANNEL;
    static constexpr uint8_t kUdpTransport = PACKETIA_TURN_UDP_TRANSPORT;

    // These APIs consume one complete UDP datagram, not a TCP stream.
    static bool ParseStunDatagram(const uint8_t* data, size_t len, StunMessageInfo& out);
    static bool ParseChannelDataDatagram(const uint8_t* data, size_t len, TurnChannelDataView& out);
    static bool BuildChannelData(uint16_t channel, std::string_view data,
                                 std::vector<uint8_t>& out, bool pad_to_four = false);

    static StunAttribute UInt32Attribute(AttrType type, uint32_t value);
    static StunAttribute RequestedTransportAttribute(uint8_t transport);
    static bool ChannelNumberAttribute(uint16_t channel, StunAttribute& out);
    static bool XorAddressAttribute(AttrType type, const IpEndpoint& ep,
                                    const std::array<uint8_t, PACKETIA_STUN_TRANSACTION_ID_BYTES>& txid, StunAttribute& out);

    static bool DecodeUInt32(const StunMessageInfo& msg, AttrType type, uint32_t& out);
    static bool DecodeRequestedTransport(const StunMessageInfo& msg, uint8_t& out);
    static bool DecodeChannelNumber(const StunMessageInfo& msg, uint16_t& out);
};

} // namespace protocol

#endif
