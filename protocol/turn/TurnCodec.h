#ifndef PACKETIA_TURN_CODEC_H
#define PACKETIA_TURN_CODEC_H

#include "Stun.h"

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
    static constexpr uint16_t kFirstChannel = 0x4000;
    static constexpr uint16_t kLastChannel = 0x4FFF; // RFC 8656, section 12.
    static constexpr uint8_t kUdpTransport = 17;

    // These APIs consume one complete UDP datagram, not a TCP stream.
    static bool ParseStunDatagram(const uint8_t* data, size_t len, StunMessageInfo& out);
    static bool ParseChannelDataDatagram(const uint8_t* data, size_t len, TurnChannelDataView& out);
    static bool BuildChannelData(uint16_t channel, std::string_view data,
                                 std::vector<uint8_t>& out, bool pad_to_four = false);

    static StunAttribute UInt32Attribute(AttrType type, uint32_t value);
    static StunAttribute RequestedTransportAttribute(uint8_t transport);
    static bool ChannelNumberAttribute(uint16_t channel, StunAttribute& out);
    static bool XorAddressAttribute(AttrType type, const IpEndpoint& ep,
                                    const std::array<uint8_t, 12>& txid, StunAttribute& out);

    static bool DecodeUInt32(const StunMessageInfo& msg, AttrType type, uint32_t& out);
    static bool DecodeRequestedTransport(const StunMessageInfo& msg, uint8_t& out);
    static bool DecodeChannelNumber(const StunMessageInfo& msg, uint16_t& out);
};

} // namespace protocol

#endif
