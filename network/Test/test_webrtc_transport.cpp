#include "WebRtcTransport.h"

#include <gtest/gtest.h>

#include <array>
#include <utility>

namespace
{

class FakeDatagramTransport final
    : public network::transport::IDatagramTransport
{
public:
    uint64_t Id() const noexcept override { return 10; }
    bool IsWritable() const noexcept override { return writable; }

    network::transport::DatagramSendResult SendDatagram(
        const network::SocketAddr& remote,
        const uint8_t* data,
        size_t size) override
    {
        if (!writable) return network::transport::DatagramSendResult::Closed;
        last_remote = remote;
        last_payload.assign(data, data + size);
        return network::transport::DatagramSendResult::Ok;
    }

    void SetDatagramSink(
        std::weak_ptr<network::transport::IDatagramSink> value) override
    {
        sink = std::move(value);
    }

    void Close() override { writable = false; }

    void Deliver(const network::SocketAddr& remote,
                 const uint8_t* data,
                 size_t size)
    {
        if (auto target = sink.lock())
        {
            target->OnDatagram(network::transport::ReceivedDatagram(
                Id(), 123, remote, data, size));
        }
    }

    bool writable = true;
    network::SocketAddr last_remote{};
    std::vector<uint8_t> last_payload;
    std::weak_ptr<network::transport::IDatagramSink> sink;
};

class FakeWebRtcSink final : public protocol::webrtc::IWebRtcTransportSink
{
public:
    void OnWebRtcDatagram(
        network::transport::DatagramProtocol value,
        network::transport::ReceivedDatagram) override
    {
        protocol = value;
        ++packet_count;
    }

    network::transport::DatagramProtocol protocol =
        network::transport::DatagramProtocol::Unknown;
    size_t packet_count = 0;
};

TEST(WebRtcTransportTest, AcceptsStunBeforePeerSelection)
{
    auto datagram = std::make_shared<FakeDatagramTransport>();
    auto sink = std::make_shared<FakeWebRtcSink>();
    auto transport = std::make_shared<protocol::webrtc::WebRtcTransport>(20, datagram);
    transport->SetSink(sink);
    ASSERT_TRUE(transport->Start());

    std::array<uint8_t, 20> stun{};
    stun[4] = 0x21;
    stun[5] = 0x12;
    stun[6] = 0xA4;
    stun[7] = 0x42;
    const auto peer = network::SocketAddr::FromIPPort("127.0.0.1", 5000);
    datagram->Deliver(peer, stun.data(), stun.size());

    EXPECT_EQ(sink->packet_count, 1U);
    EXPECT_EQ(sink->protocol, network::transport::DatagramProtocol::Stun);
}

TEST(WebRtcTransportTest, RejectsMediaUntilPeerIsSelected)
{
    auto datagram = std::make_shared<FakeDatagramTransport>();
    auto sink = std::make_shared<FakeWebRtcSink>();
    auto transport = std::make_shared<protocol::webrtc::WebRtcTransport>(20, datagram);
    transport->SetSink(sink);
    ASSERT_TRUE(transport->Start());

    std::array<uint8_t, 12> rtp{};
    rtp[0] = 0x80;
    rtp[1] = 96;
    const auto peer = network::SocketAddr::FromIPPort("127.0.0.1", 5000);
    datagram->Deliver(peer, rtp.data(), rtp.size());
    EXPECT_EQ(sink->packet_count, 0U);

    ASSERT_TRUE(transport->SelectPeer(peer));
    datagram->Deliver(peer, rtp.data(), rtp.size());
    EXPECT_EQ(sink->packet_count, 1U);
    EXPECT_EQ(sink->protocol, network::transport::DatagramProtocol::Rtp);
}

} // namespace
