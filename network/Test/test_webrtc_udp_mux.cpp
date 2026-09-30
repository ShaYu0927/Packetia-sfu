#include "WebRtcUdpMux.h"
#include "Stun.h"

#include <gtest/gtest.h>

#include <functional>
#include <utility>
#include <vector>

namespace
{

class DatagramCollector final : public network::transport::IDatagramSink
{
public:
    void OnDatagram(network::transport::ReceivedDatagram datagram) override
    {
        packets.push_back(std::move(datagram));
        if (on_packet) on_packet();
    }

    std::vector<network::transport::ReceivedDatagram> packets;
    std::function<void()> on_packet;
};

std::vector<uint8_t> BindingRequest(const std::string& username)
{
    protocol::IceRequestParams request;
    request.username = username;
    request.password = std::string(24, 'p');
    request.controlling = true;
    request.tie_breaker = 1;
    request.priority = 1234;
    std::vector<uint8_t> packet(1500);
    size_t size = 0;
    EXPECT_TRUE(protocol::StunCodec::BuildIceBindingRequest(
        request, packet.data(), packet.size(), size));
    packet.resize(size);
    return packet;
}

std::vector<uint8_t> Rtp()
{
    std::vector<uint8_t> packet(12);
    packet[0] = 0x80;
    packet[1] = 111;
    return packet;
}

class WebRtcUdpMuxTest : public testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(loop.Start());
        scheduler = loop.GetTaskScheduler();
        server = std::make_shared<network::UdpServer>(scheduler);
        ASSERT_TRUE(server->Start("127.0.0.1", 0));
        mux = std::make_shared<protocol::webrtc::WebRtcUdpMux>(server);
        server->SetHandler(mux);
    }

    void TearDown() override
    {
        if (server) server->Stop();
        mux.reset();
        server.reset();
        loop.Stop();
    }

    void Deliver(const network::SocketAddr& source, const std::vector<uint8_t>& packet)
    {
        mux->OnDatagram(source, packet.data(), packet.size());
    }

    EventLoop loop{1};
    std::shared_ptr<TaskScheduler> scheduler;
    std::shared_ptr<network::UdpServer> server;
    std::shared_ptr<protocol::webrtc::WebRtcUdpMux> mux;
    const network::SocketAddr alice = network::SocketAddr::FromIPPort("127.0.0.1", 50101);
    const network::SocketAddr bob = network::SocketAddr::FromIPPort("127.0.0.1", 50102);
};

TEST_F(WebRtcUdpMuxTest, InterleavedChecksUseUfragAndNeverBindMediaByThemselves)
{
    scheduler->Invoke([&] {
        auto first = mux->Register(1, "first");
        auto second = mux->Register(2, "second");
        auto first_sink = std::make_shared<DatagramCollector>();
        auto second_sink = std::make_shared<DatagramCollector>();
        ASSERT_TRUE(first && second);
        first->SetDatagramSink(first_sink);
        second->SetDatagramSink(second_sink);

        Deliver(bob, BindingRequest("second:remoteB"));
        Deliver(alice, BindingRequest("first:remoteA"));
        Deliver(bob, BindingRequest("second:remoteB"));
        ASSERT_EQ(first_sink->packets.size(), 1U);
        ASSERT_EQ(second_sink->packets.size(), 2U);
        EXPECT_EQ(first_sink->packets.front().transport_id, 1U);
        EXPECT_EQ(second_sink->packets.front().transport_id, 2U);

        Deliver(alice, Rtp());
        Deliver(bob, Rtp());
        EXPECT_EQ(first_sink->packets.size(), 1U);
        EXPECT_EQ(second_sink->packets.size(), 2U);

        ASSERT_TRUE(mux->BindPeer("first", alice));
        // Even an already selected address must route STUN by USERNAME.
        Deliver(alice, BindingRequest("second:remoteB"));
        Deliver(alice, BindingRequest("unknown:remote"));
        EXPECT_EQ(first_sink->packets.size(), 1U);
        EXPECT_EQ(second_sink->packets.size(), 3U);
        Deliver(alice, Rtp());
        EXPECT_EQ(first_sink->packets.size(), 2U);
        EXPECT_EQ(second_sink->packets.size(), 3U);
    });
}

TEST_F(WebRtcUdpMuxTest, RejectsMalformedStunAndDuplicateRegistrations)
{
    scheduler->Invoke([&] {
        auto transport = mux->Register(1, "local");
        auto sink = std::make_shared<DatagramCollector>();
        ASSERT_TRUE(transport);
        transport->SetDatagramSink(sink);
        EXPECT_FALSE(mux->Register(2, "local"));
        EXPECT_FALSE(mux->Register(0, "other"));
        EXPECT_FALSE(mux->Register(3, "bad:ufrag"));
        ASSERT_TRUE(mux->BindPeer("local", alice));

        for (const auto* username : {"local", ":remote", "local:", "local:remote:extra", "other:remote"})
            Deliver(alice, BindingRequest(username));
        auto truncated = BindingRequest("local:remote");
        truncated.pop_back();
        Deliver(alice, truncated);
        auto invalid_attribute = BindingRequest("local:remote");
        invalid_attribute[22] = 0xff;
        invalid_attribute[23] = 0xff;
        Deliver(alice, invalid_attribute);
        auto trailing_data = BindingRequest("local:remote");
        trailing_data.push_back(0);
        Deliver(alice, trailing_data);
        auto response = BindingRequest("local:remote");
        response[0] = 1; // Binding success is not an incoming ICE-lite check.
        Deliver(alice, response);
        EXPECT_TRUE(sink->packets.empty());

        Deliver(alice, BindingRequest("local:remote"));
        EXPECT_EQ(sink->packets.size(), 1U);
        EXPECT_TRUE(transport->IsWritable());
    });
}

TEST_F(WebRtcUdpMuxTest, BindingConflictsAndPeerChangesPreserveSessionIsolation)
{
    scheduler->Invoke([&] {
        auto first = mux->Register(1, "first");
        auto second = mux->Register(2, "second");
        auto first_sink = std::make_shared<DatagramCollector>();
        auto second_sink = std::make_shared<DatagramCollector>();
        ASSERT_TRUE(first && second);
        first->SetDatagramSink(first_sink);
        second->SetDatagramSink(second_sink);
        ASSERT_TRUE(mux->BindPeer("first", alice));
        EXPECT_TRUE(mux->BindPeer("first", alice));
        EXPECT_FALSE(mux->BindPeer("second", alice));
        EXPECT_FALSE(mux->BindPeer("missing", bob));
        EXPECT_FALSE(mux->BindPeer("first", {}));
        Deliver(alice, Rtp());
        EXPECT_EQ(first_sink->packets.size(), 1U);
        EXPECT_TRUE(second_sink->packets.empty());

        ASSERT_TRUE(mux->BindPeer("first", bob));
        Deliver(alice, Rtp());
        EXPECT_EQ(first_sink->packets.size(), 1U);
        ASSERT_TRUE(mux->BindPeer("second", alice));
        Deliver(bob, Rtp());
        Deliver(alice, Rtp());
        EXPECT_EQ(first_sink->packets.size(), 2U);
        EXPECT_EQ(second_sink->packets.size(), 1U);
    });
}

TEST_F(WebRtcUdpMuxTest, UnregisterClosesOnlyOneAdapterAndRemovesBothMappings)
{
    scheduler->Invoke([&] {
        auto first = mux->Register(1, "first");
        auto second = mux->Register(2, "second");
        auto first_sink = std::make_shared<DatagramCollector>();
        auto second_sink = std::make_shared<DatagramCollector>();
        ASSERT_TRUE(first && second);
        first->SetDatagramSink(first_sink);
        second->SetDatagramSink(second_sink);
        ASSERT_TRUE(mux->BindPeer("first", alice));
        ASSERT_TRUE(mux->BindPeer("second", bob));
        mux->Unregister("first");
        mux->Unregister("first");
        EXPECT_FALSE(first->IsWritable());
        EXPECT_TRUE(second->IsWritable());
        EXPECT_TRUE(server->IsWritable());
        Deliver(alice, BindingRequest("first:remote"));
        Deliver(alice, Rtp());
        Deliver(bob, Rtp());
        EXPECT_TRUE(first_sink->packets.empty());
        EXPECT_EQ(second_sink->packets.size(), 1U);

        auto replacement = mux->Register(3, "first");
        ASSERT_TRUE(replacement);
        auto replacement_sink = std::make_shared<DatagramCollector>();
        replacement->SetDatagramSink(replacement_sink);
        Deliver(alice, Rtp());
        EXPECT_TRUE(replacement_sink->packets.empty());
        ASSERT_TRUE(mux->BindPeer("first", alice));
        Deliver(alice, Rtp());
        EXPECT_EQ(replacement_sink->packets.size(), 1U);

        // A retained but explicitly closed adapter cannot reserve an address.
        replacement->Close();
        EXPECT_TRUE(mux->BindPeer("second", alice));
        mux->Unregister("first");
        Deliver(alice, Rtp());
        EXPECT_EQ(second_sink->packets.size(), 2U);
    });
}

TEST_F(WebRtcUdpMuxTest, CallbackMayUnregisterItsOwnSessionAndCloseClearsAllAdapters)
{
    scheduler->Invoke([&] {
        auto first = mux->Register(1, "first");
        auto sink = std::make_shared<DatagramCollector>();
        ASSERT_TRUE(first);
        first->SetDatagramSink(sink);
        sink->on_packet = [&] { mux->Unregister("first"); };
        Deliver(alice, BindingRequest("first:remote"));
        EXPECT_EQ(sink->packets.size(), 1U);
        EXPECT_FALSE(first->IsWritable());

        auto second = mux->Register(2, "second");
        ASSERT_TRUE(second);
        ASSERT_TRUE(mux->BindPeer("second", bob));
        mux->Close();
        mux->Close();
        EXPECT_FALSE(second->IsWritable());
        EXPECT_TRUE(server->IsWritable());
        EXPECT_FALSE(mux->Register(3, "third"));
        EXPECT_FALSE(mux->BindPeer("second", alice));
    });
}

} // namespace
