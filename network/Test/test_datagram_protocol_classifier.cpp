#include "transport/DatagramProtocolClassifier.h"

#include <gtest/gtest.h>

#include <array>

namespace
{
using network::transport::DatagramProtocol;
using network::transport::DatagramProtocolClassifier;

TEST(DatagramProtocolClassifierTest, ClassifiesStunByMagicCookie)
{
    std::array<uint8_t, 20> packet{};
    packet[4] = 0x21;
    packet[5] = 0x12;
    packet[6] = 0xA4;
    packet[7] = 0x42;

    EXPECT_EQ(DatagramProtocolClassifier::Classify(packet.data(), packet.size()),
              DatagramProtocol::Stun);
}

TEST(DatagramProtocolClassifierTest, ClassifiesDtlsRecord)
{
    std::array<uint8_t, 13> packet{};
    packet[0] = 22;
    packet[1] = 0xFE;
    packet[2] = 0xFD;

    EXPECT_EQ(DatagramProtocolClassifier::Classify(packet.data(), packet.size()),
              DatagramProtocol::Dtls);
}

TEST(DatagramProtocolClassifierTest, DistinguishesRtpAndRtcp)
{
    std::array<uint8_t, 12> rtp{};
    rtp[0] = 0x80;
    rtp[1] = 96;

    std::array<uint8_t, 8> rtcp{};
    rtcp[0] = 0x80;
    rtcp[1] = 200;

    EXPECT_EQ(DatagramProtocolClassifier::Classify(rtp.data(), rtp.size()),
              DatagramProtocol::Rtp);
    EXPECT_EQ(DatagramProtocolClassifier::Classify(rtcp.data(), rtcp.size()),
              DatagramProtocol::Rtcp);
}

TEST(DatagramProtocolClassifierTest, RejectsTruncatedAndUnknownPackets)
{
    const std::array<uint8_t, 3> truncated{{0x80, 96, 0}};
    const std::array<uint8_t, 13> unknown{{0x10}};

    EXPECT_EQ(DatagramProtocolClassifier::Classify(nullptr, 0),
              DatagramProtocol::Unknown);
    EXPECT_EQ(DatagramProtocolClassifier::Classify(truncated.data(), truncated.size()),
              DatagramProtocol::Unknown);
    EXPECT_EQ(DatagramProtocolClassifier::Classify(unknown.data(), unknown.size()),
              DatagramProtocol::Unknown);
}

} // namespace
