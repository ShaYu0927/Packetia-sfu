#include "rtmp_session.h"

#include <gtest/gtest.h>

#include <utility>
#include <vector>

namespace
{

class FakeRtmpTransport final : public protocol::rtmp::IRtmpTransport
{
public:
    protocol::rtmp::RtmpTransportState State() const noexcept override
    {
        return state;
    }

    bool Start(std::weak_ptr<protocol::rtmp::IRtmpTransportSink> value) override
    {
        sink = std::move(value);
        state = protocol::rtmp::RtmpTransportState::Connected;
        return true;
    }

    protocol::rtmp::RtmpTransportSendResult Send(
        const uint8_t* data,
        size_t size) override
    {
        if (state != protocol::rtmp::RtmpTransportState::Connected)
        {
            return protocol::rtmp::RtmpTransportSendResult::NotWritable;
        }
        sent.insert(sent.end(), data, data + size);
        return protocol::rtmp::RtmpTransportSendResult::Ok;
    }

    void Close() override
    {
        state = protocol::rtmp::RtmpTransportState::Closed;
        sink.reset();
    }

    protocol::rtmp::RtmpTransportState state =
        protocol::rtmp::RtmpTransportState::Created;
    std::weak_ptr<protocol::rtmp::IRtmpTransportSink> sink;
    std::vector<uint8_t> sent;
};

TEST(RtmpTransportTest, ClientSessionStartsHandshakeThroughTransport)
{
    auto transport = std::make_shared<FakeRtmpTransport>();
    auto session = std::make_shared<protocol::rtmp::RtmpSession>(
        transport, protocol::rtmp::EndpointRole::kClient);

    session->Start();

    EXPECT_EQ(transport->state,
              protocol::rtmp::RtmpTransportState::Connected);
    ASSERT_EQ(transport->sent.size(), 1537U);
    EXPECT_EQ(transport->sent.front(), 3U);
}

} // namespace
