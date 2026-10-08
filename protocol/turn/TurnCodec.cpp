#include "TurnCodec.h"
#include "utils.h"

#include <utility>

namespace protocol
{
namespace
{
bool ValidChannel(uint16_t channel)
{
    return channel >= TurnCodec::kFirstChannel && channel <= TurnCodec::kLastChannel;
}

const AttrView* FourByteAttribute(const StunMessageInfo& msg, AttrType type)
{
    const auto* attr = msg.FindAttr(static_cast<uint16_t>(type));
    if (!attr || attr->len != 4 || msg.AttrValue(*attr).size() != 4) return nullptr;
    return attr;
}
} // namespace

bool TurnCodec::ParseStunDatagram(const uint8_t* data, size_t len, StunMessageInfo& out)
{
    out = {};
    StunMessageInfo parsed;
    if (!StunCodec::Parse(data, len, parsed) || parsed.raw_len != len) return false;
    out = std::move(parsed);
    return true;
}

bool TurnCodec::ParseChannelDataDatagram(const uint8_t* data, size_t len, TurnChannelDataView& out)
{
    out = {};
    if (!data || len < 4) return false;
    const uint16_t channel = utils::Utils::ReadUint16BE(data);
    const size_t payload_size = utils::Utils::ReadUint16BE(data + 2);
    if (!ValidChannel(channel) || payload_size > len - 4) return false;

    // UDP may carry padding. It is outside the declared application data.
    out.channel = channel;
    out.data = {reinterpret_cast<const char*>(data + 4), payload_size};
    return true;
}

bool TurnCodec::BuildChannelData(uint16_t channel, std::string_view data,
                                 std::vector<uint8_t>& out, bool pad_to_four)
{
    out.clear();
    if (!ValidChannel(channel) || data.size() > 0xFFFF) return false;
    std::vector<uint8_t> buf(4, 0);
    utils::Utils::WriteUint16BE(buf.data(), channel);
    utils::Utils::WriteUint16BE(buf.data() + 2, static_cast<uint16_t>(data.size()));
    if (!data.empty()) buf.insert(buf.end(), data.begin(), data.end());
    if (pad_to_four) buf.resize((buf.size() + 3u) & ~size_t(3u), 0);
    out.swap(buf);
    return true;
}

StunAttribute TurnCodec::UInt32Attribute(AttrType type, uint32_t value)
{
    StunAttribute attr{static_cast<uint16_t>(type), std::vector<uint8_t>(4, 0)};
    utils::Utils::WriteUint32BE(attr.value.data(), value);
    return attr;
}

StunAttribute TurnCodec::RequestedTransportAttribute(uint8_t transport)
{
    return {static_cast<uint16_t>(AttrType::REQUESTED_TRANSPORT), {transport, 0, 0, 0}};
}

bool TurnCodec::ChannelNumberAttribute(uint16_t channel, StunAttribute& out)
{
    out = {};
    if (!ValidChannel(channel)) return false;
    out = {static_cast<uint16_t>(AttrType::CHANNEL_NUMBER), std::vector<uint8_t>(4, 0)};
    utils::Utils::WriteUint16BE(out.value.data(), channel);
    return true;
}

bool TurnCodec::XorAddressAttribute(AttrType type, const IpEndpoint& ep,
                                    const std::array<uint8_t, 12>& txid, StunAttribute& out)
{
    out = {};
    if (type != AttrType::XOR_MAPPED_ADDRESS && type != AttrType::XOR_PEER_ADDRESS &&
        type != AttrType::XOR_RELAYED_ADDRESS) return false;
    if (!StunCodec::EncodeXorAddress(ep, txid, out.value)) return false;
    out.type = static_cast<uint16_t>(type);
    return true;
}

bool TurnCodec::DecodeUInt32(const StunMessageInfo& msg, AttrType type, uint32_t& out)
{
    out = 0;
    const auto* attr = FourByteAttribute(msg, type);
    if (!attr) return false;
    out = utils::Utils::ReadUint32BE(msg.raw + attr->value_offset);
    return true;
}

bool TurnCodec::DecodeRequestedTransport(const StunMessageInfo& msg, uint8_t& out)
{
    out = 0;
    const auto* attr = FourByteAttribute(msg, AttrType::REQUESTED_TRANSPORT);
    if (!attr) return false;
    // Reserved bytes are ignored on receipt, as required by RFC 8656.
    out = msg.raw[attr->value_offset];
    return true;
}

bool TurnCodec::DecodeChannelNumber(const StunMessageInfo& msg, uint16_t& out)
{
    out = 0;
    const auto* attr = FourByteAttribute(msg, AttrType::CHANNEL_NUMBER);
    if (!attr) return false;
    const auto channel = utils::Utils::ReadUint16BE(msg.raw + attr->value_offset);
    if (!ValidChannel(channel)) return false;
    out = channel;
    return true;
}

} // namespace protocol
