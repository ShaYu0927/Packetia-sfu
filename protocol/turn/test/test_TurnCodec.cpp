#include <gtest/gtest.h>

#include <algorithm>
#include "TurnCodec.h"

namespace protocol
{
namespace
{
const std::array<uint8_t, 12> kTxid = {
    0xb7, 0xe7, 0xa7, 0x01, 0xbc, 0x34, 0xd6, 0x86, 0xfa, 0x87, 0xdf, 0xae
};

StunAttribute TextAttribute(AttrType type, std::string_view text)
{
    return {static_cast<uint16_t>(type), {text.begin(), text.end()}};
}
} // namespace

TEST(TurnCodecTest, AllocateMatchesFixedWireLayout)
{
    const std::vector<uint8_t> expected = {
        0x00,0x03,0x00,0x10,0x21,0x12,0xa4,0x42,
        0xb7,0xe7,0xa7,0x01,0xbc,0x34,0xd6,0x86,0xfa,0x87,0xdf,0xae,
        0x00,0x19,0x00,0x04,0x11,0x00,0x00,0x00,
        0x00,0x0d,0x00,0x04,0x00,0x00,0x02,0x58
    };
    std::vector<uint8_t> packet;
    ASSERT_TRUE(StunCodec::BuildMessage(StunMethod::Allocate, StunClass::Request, kTxid,
        {TurnCodec::RequestedTransportAttribute(TurnCodec::kUdpTransport),
         TurnCodec::UInt32Attribute(AttrType::LIFETIME, 600)}, packet));
    EXPECT_EQ(packet, expected);

    StunMessageInfo msg;
    ASSERT_TRUE(TurnCodec::ParseStunDatagram(expected.data(), expected.size(), msg));
    EXPECT_EQ(msg.method, StunMethod::Allocate);
    EXPECT_EQ(msg.klass, StunClass::Request);
    uint8_t transport = 0;
    uint32_t lifetime = 0;
    ASSERT_TRUE(TurnCodec::DecodeRequestedTransport(msg, transport));
    ASSERT_TRUE(TurnCodec::DecodeUInt32(msg, AttrType::LIFETIME, lifetime));
    EXPECT_EQ(transport, 17);
    EXPECT_EQ(lifetime, 600u);
}

TEST(TurnCodecTest, TurnMethodsHaveCorrectMessageTypes)
{
    const std::pair<StunMethod, uint8_t> methods[] = {
        {StunMethod::Allocate, 0x03}, {StunMethod::Refresh, 0x04},
        {StunMethod::Send, 0x06}, {StunMethod::Data, 0x07},
        {StunMethod::CreatePermission, 0x08}, {StunMethod::ChannelBind, 0x09}
    };
    for (const auto& item : methods)
    {
        for (const auto klass : {StunClass::Request, StunClass::Indication,
                                 StunClass::SuccessResponse, StunClass::ErrorResponse})
        {
            std::vector<uint8_t> packet;
            ASSERT_TRUE(StunCodec::BuildMessage(item.first, klass, kTxid, {}, packet));
            const auto value = static_cast<uint8_t>(klass);
            EXPECT_EQ(packet[0], (value >> 1) & 1);
            EXPECT_EQ(packet[1], item.second | ((value & 1) << 4));
        }
    }
}

TEST(TurnCodecTest, SignedMessageProtectsCredentialsAndUsesBinaryKey)
{
    const std::string key("\x00\x11\x22\x00\x44", 5);
    std::vector<uint8_t> packet;
    ASSERT_TRUE(StunCodec::BuildMessage(StunMethod::Allocate, StunClass::Request, kTxid,
        {TextAttribute(AttrType::USERNAME, "alice"), TextAttribute(AttrType::REALM, "example.org"),
         TextAttribute(AttrType::NONCE, "nonce"),
         TurnCodec::RequestedTransportAttribute(17)}, packet, key, true));
    StunMessageInfo msg;
    ASSERT_TRUE(TurnCodec::ParseStunDatagram(packet.data(), packet.size(), msg));
    EXPECT_TRUE(StunCodec::VerifyMessageIntegrity(msg, key));
    EXPECT_FALSE(StunCodec::VerifyMessageIntegrity(msg, "wrong"));
    EXPECT_TRUE(StunCodec::VerifyFingerprint(msg));
    EXPECT_EQ(msg.attrs[msg.attrs.size() - 2].type, static_cast<uint16_t>(AttrType::MESSAGE_INTEGRITY));
    EXPECT_EQ(msg.attrs.back().type, static_cast<uint16_t>(AttrType::FINGERPRINT));

    packet[24] ^= 1; // Change the first USERNAME byte without updating authentication.
    ASSERT_TRUE(TurnCodec::ParseStunDatagram(packet.data(), packet.size(), msg));
    EXPECT_FALSE(StunCodec::VerifyMessageIntegrity(msg, key));
    EXPECT_FALSE(StunCodec::VerifyFingerprint(msg));
}

TEST(TurnCodecTest, IPv4PeerAddressMatchesRfc5769Encoding)
{
    IpEndpoint ep;
    ep.port = 32853;
    ep.ip = {192, 0, 2, 1};
    StunAttribute attr;
    ASSERT_TRUE(TurnCodec::XorAddressAttribute(AttrType::XOR_PEER_ADDRESS, ep, kTxid, attr));
    EXPECT_EQ(attr.value, (std::vector<uint8_t>{0x00,0x01,0xa1,0x47,0xe1,0x12,0xa6,0x43}));
    std::vector<uint8_t> packet;
    ASSERT_TRUE(StunCodec::BuildMessage(StunMethod::CreatePermission, StunClass::Request,
                                       kTxid, {attr}, packet));
    StunMessageInfo msg;
    ASSERT_TRUE(TurnCodec::ParseStunDatagram(packet.data(), packet.size(), msg));
    XorMappedAddress decoded;
    ASSERT_TRUE(StunCodec::DecodeXorAddress(msg, AttrType::XOR_PEER_ADDRESS, decoded));
    EXPECT_FALSE(decoded.is_ipv6);
    EXPECT_EQ(decoded.port, ep.port);
    EXPECT_EQ(decoded.ip, ep.ip);
}

TEST(TurnCodecTest, IPv6RelayAddressMatchesRfc5769Encoding)
{
    IpEndpoint ep;
    ep.family = IpFamily::IPv6;
    ep.port = 32853;
    ep.ip = {0x20,0x01,0x0d,0xb8,0x12,0x34,0x56,0x78,
             0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77};
    StunAttribute attr;
    ASSERT_TRUE(TurnCodec::XorAddressAttribute(AttrType::XOR_RELAYED_ADDRESS, ep, kTxid, attr));
    EXPECT_EQ(attr.value, (std::vector<uint8_t>{
        0x00,0x02,0xa1,0x47,0x01,0x13,0xa9,0xfa,0xa5,0xd3,0xf1,0x79,
        0xbc,0x25,0xf4,0xb5,0xbe,0xd2,0xb9,0xd9
    }));
    std::vector<uint8_t> packet;
    ASSERT_TRUE(StunCodec::BuildMessage(StunMethod::Allocate, StunClass::SuccessResponse,
                                       kTxid, {attr}, packet));
    StunMessageInfo msg;
    ASSERT_TRUE(TurnCodec::ParseStunDatagram(packet.data(), packet.size(), msg));
    XorMappedAddress decoded;
    ASSERT_TRUE(StunCodec::DecodeXorAddress(msg, AttrType::XOR_RELAYED_ADDRESS, decoded));
    EXPECT_TRUE(decoded.is_ipv6);
    EXPECT_EQ(decoded.port, ep.port);
    EXPECT_EQ(decoded.ip, ep.ip);
}

TEST(TurnCodecTest, ChannelDataHasFourByteHeaderAndOpaqueBinaryPayload)
{
    const std::string payload("a\0b", 3);
    for (bool padding : {false, true})
    {
        std::vector<uint8_t> packet;
        ASSERT_TRUE(TurnCodec::BuildChannelData(0x4001, payload, packet, padding));
        EXPECT_EQ(packet.size(), padding ? 8u : 7u);
        EXPECT_TRUE(std::equal(packet.begin(), packet.begin() + 4,
                              std::vector<uint8_t>{0x40,0x01,0x00,0x03}.begin()));
        TurnChannelDataView view;
        ASSERT_TRUE(TurnCodec::ParseChannelDataDatagram(packet.data(), packet.size(), view));
        EXPECT_EQ(view.channel, 0x4001);
        EXPECT_EQ(view.data, payload);
        StunMessageInfo msg;
        EXPECT_FALSE(TurnCodec::ParseStunDatagram(packet.data(), packet.size(), msg));
    }
}

TEST(TurnCodecTest, ChannelDataRejectsTruncationAndReservedChannels)
{
    TurnChannelDataView view;
    EXPECT_FALSE(TurnCodec::ParseChannelDataDatagram(nullptr, 4, view));
    const uint8_t truncated[] = {0x40,0x00,0x00,0x02,0xaa};
    EXPECT_FALSE(TurnCodec::ParseChannelDataDatagram(truncated, sizeof(truncated), view));
    EXPECT_EQ(view.channel, 0);
    EXPECT_TRUE(view.data.empty());
    for (uint16_t channel : {0x3FFF, 0x5000, 0x7FFF, 0xFFFF})
    {
        std::vector<uint8_t> out = {1};
        EXPECT_FALSE(TurnCodec::BuildChannelData(channel, "x", out));
        EXPECT_TRUE(out.empty());
        const uint8_t packet[] = {uint8_t(channel >> 8), uint8_t(channel), 0, 0};
        EXPECT_FALSE(TurnCodec::ParseChannelDataDatagram(packet, sizeof(packet), view));
        StunAttribute attr;
        EXPECT_FALSE(TurnCodec::ChannelNumberAttribute(channel, attr));
    }
    for (uint16_t channel : {0x4000, 0x4FFF})
    {
        std::vector<uint8_t> out;
        ASSERT_TRUE(TurnCodec::BuildChannelData(channel, {}, out));
        ASSERT_TRUE(TurnCodec::ParseChannelDataDatagram(out.data(), out.size(), view));
        EXPECT_EQ(view.channel, channel);
        EXPECT_TRUE(view.data.empty());
    }
    std::vector<uint8_t> out;
    EXPECT_FALSE(TurnCodec::BuildChannelData(0x4000, std::string(65536, 'x'), out));
}

TEST(TurnCodecTest, DatagramParsingRejectsExtraStunBytesAndMalformedAttributes)
{
    std::vector<uint8_t> packet;
    ASSERT_TRUE(StunCodec::BuildMessage(StunMethod::Refresh, StunClass::Request,
        kTxid, {TurnCodec::UInt32Attribute(AttrType::LIFETIME, 0)}, packet));
    StunMessageInfo msg;
    ASSERT_TRUE(TurnCodec::ParseStunDatagram(packet.data(), packet.size(), msg));
    uint32_t lifetime = 1;
    ASSERT_TRUE(TurnCodec::DecodeUInt32(msg, AttrType::LIFETIME, lifetime));
    EXPECT_EQ(lifetime, 0u); // Refresh with zero lifetime requests deletion.
    packet.push_back(0);
    EXPECT_FALSE(TurnCodec::ParseStunDatagram(packet.data(), packet.size(), msg));
    EXPECT_EQ(msg.raw, nullptr);
    packet.pop_back();
    packet[23] = 8; // Attribute claims a value beyond the message boundary.
    EXPECT_FALSE(TurnCodec::ParseStunDatagram(packet.data(), packet.size(), msg));
}

TEST(TurnCodecTest, AttributeDecodersValidateLengthAndIgnoreReservedBytes)
{
    StunAttribute channel;
    ASSERT_TRUE(TurnCodec::ChannelNumberAttribute(0x4001, channel));
    channel.value[2] = 0xFF;
    auto transport = TurnCodec::RequestedTransportAttribute(17);
    transport.value[1] = 0xFF;
    std::vector<uint8_t> packet;
    ASSERT_TRUE(StunCodec::BuildMessage(StunMethod::ChannelBind, StunClass::Request,
                                       kTxid, {channel, transport}, packet));
    StunMessageInfo msg;
    ASSERT_TRUE(TurnCodec::ParseStunDatagram(packet.data(), packet.size(), msg));
    uint16_t number = 0;
    uint8_t protocol = 0;
    ASSERT_TRUE(TurnCodec::DecodeChannelNumber(msg, number));
    ASSERT_TRUE(TurnCodec::DecodeRequestedTransport(msg, protocol));
    EXPECT_EQ(number, 0x4001);
    EXPECT_EQ(protocol, 17);

    channel.value.resize(2);
    ASSERT_TRUE(StunCodec::BuildMessage(StunMethod::ChannelBind, StunClass::Request,
                                       kTxid, {channel}, packet));
    ASSERT_TRUE(TurnCodec::ParseStunDatagram(packet.data(), packet.size(), msg));
    EXPECT_FALSE(TurnCodec::DecodeChannelNumber(msg, number));
    EXPECT_EQ(number, 0);
}

TEST(TurnCodecTest, BuilderRejectsOverflowAndCallerSuppliedIntegrityAttributes)
{
    std::vector<uint8_t> out;
    StunAttribute data{static_cast<uint16_t>(AttrType::DATA), std::vector<uint8_t>(65528)};
    EXPECT_TRUE(StunCodec::BuildMessage(StunMethod::Send, StunClass::Indication, kTxid, {data}, out));
    EXPECT_EQ(out.size(), 65552u); // Maximum aligned STUN body is 65532 bytes.
    EXPECT_FALSE(StunCodec::BuildMessage(StunMethod::Send, StunClass::Indication,
                                        kTxid, {data}, out, {}, true));
    EXPECT_TRUE(out.empty());
    data.value.resize(65529);
    EXPECT_FALSE(StunCodec::BuildMessage(StunMethod::Send, StunClass::Indication, kTxid, {data}, out));
    data.value.resize(65536);
    EXPECT_FALSE(StunCodec::BuildMessage(StunMethod::Send, StunClass::Indication, kTxid, {data}, out));
    for (auto type : {AttrType::MESSAGE_INTEGRITY, AttrType::MESSAGE_INTEGRITY_SHA256, AttrType::FINGERPRINT})
    {
        EXPECT_FALSE(StunCodec::BuildMessage(StunMethod::Allocate, StunClass::Request,
                                            kTxid, {{static_cast<uint16_t>(type), {}}}, out));
    }
    EXPECT_FALSE(StunCodec::BuildMessage(static_cast<StunMethod>(0x1000), StunClass::Request, kTxid, {}, out));
    EXPECT_FALSE(StunCodec::BuildMessage(StunMethod::Allocate, static_cast<StunClass>(4), kTxid, {}, out));
    IpEndpoint ep;
    ep.family = static_cast<IpFamily>(5);
    StunAttribute attr;
    EXPECT_FALSE(TurnCodec::XorAddressAttribute(AttrType::XOR_PEER_ADDRESS, ep, kTxid, attr));
}

TEST(TurnCodecTest, AttributesAfterIntegrityCannotControlTurnOperations)
{
    std::vector<uint8_t> packet;
    ASSERT_TRUE(StunCodec::BuildMessage(StunMethod::Refresh, StunClass::Request,
                                       kTxid, {}, packet, "key"));
    const uint8_t lifetime[] = {0x00,0x0d,0x00,0x04,0,0,0,0};
    packet.insert(packet.end(), std::begin(lifetime), std::end(lifetime));
    packet[2] = uint8_t((packet.size() - 20) >> 8);
    packet[3] = uint8_t(packet.size() - 20);
    StunMessageInfo msg;
    ASSERT_TRUE(TurnCodec::ParseStunDatagram(packet.data(), packet.size(), msg));
    EXPECT_TRUE(StunCodec::VerifyMessageIntegrity(msg, "key"));
    uint32_t value = 600;
    EXPECT_FALSE(TurnCodec::DecodeUInt32(msg, AttrType::LIFETIME, value));
}

} // namespace protocol
