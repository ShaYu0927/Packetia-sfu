#include <gtest/gtest.h>
#include "TurnAuthRateLimiter.h"

namespace protocol
{
TEST(TurnAuthRateLimiterTest, SharesPortAndMappedAddressBudgetAndResetsAtBoundary)
{
    TurnAuthRateLimiter limiter;
    const auto first = network::SocketAddr::FromIPPort("192.0.2.1", 1234);
    const auto port = network::SocketAddr::FromIPPort("192.0.2.1", 4321);
    const auto mapped = network::SocketAddr::FromIPPort("::ffff:192.0.2.1", 5678);
    EXPECT_FALSE(limiter.Consume(first, 250, 2).suppressed);
    EXPECT_FALSE(limiter.Consume(port, 250, 2).suppressed);
    auto decision = limiter.Consume(mapped, 1249, 2);
    EXPECT_TRUE(decision.suppressed);
    EXPECT_TRUE(decision.first_drop);
    EXPECT_FALSE(decision.collision);
    EXPECT_FALSE(limiter.Consume(port, 1249, 2).first_drop);
    EXPECT_EQ(limiter.OccupiedBuckets(1249), 1u);
    EXPECT_EQ(limiter.OccupiedBuckets(1250), 0u);
    EXPECT_FALSE(limiter.Consume(mapped, 1250, 2).suppressed);
    EXPECT_FALSE(limiter.Consume(first, 1250, 2).suppressed);
    EXPECT_TRUE(limiter.Consume(port, 1250, 2).first_drop);
    limiter.Reset();
    EXPECT_EQ(limiter.OccupiedBuckets(1250), 0u);
    EXPECT_FALSE(limiter.Consume(first, 1250, 2).suppressed);
}

TEST(TurnAuthRateLimiterTest, IPv6PortsShareBudgetAndOtherAddressesAreIndependent)
{
    TurnAuthRateLimiter limiter;
    EXPECT_FALSE(limiter.Consume(network::SocketAddr::FromIPPort("2001:db8::1", 1000), 0, 1).suppressed);
    EXPECT_TRUE(limiter.Consume(network::SocketAddr::FromIPPort("2001:db8::1", 2000), 0, 1).suppressed);
    EXPECT_FALSE(limiter.Consume(network::SocketAddr::FromIPPort("2001:db8::2", 1000), 0, 1).suppressed);
    EXPECT_FALSE(limiter.Consume(network::SocketAddr::FromIPPort("192.0.2.1", 1000), 0, 1).suppressed);
}

TEST(TurnAuthRateLimiterTest, CollisionCannotReplaceAnActiveBudgetOrRepeatLogSignals)
{
    TurnAuthRateLimiter limiter;
    bool found = false;
    for (size_t i = 0; i <= TurnAuthRateLimiter::kBucketCount; ++i)
    {
        const auto ip = "198.18." + std::to_string(i / 256) + "." + std::to_string(i % 256);
        const auto source = network::SocketAddr::FromIPPort(ip, 1234);
        const auto decision = limiter.Consume(source, 0, 1);
        if (!decision.collision) continue;
        found = true;
        EXPECT_TRUE(decision.suppressed);
        EXPECT_TRUE(decision.first_collision);
        EXPECT_TRUE(decision.first_drop);
        const auto again = limiter.Consume(source, 999, 1);
        EXPECT_TRUE(again.suppressed);
        EXPECT_TRUE(again.collision);
        EXPECT_FALSE(again.first_collision);
        EXPECT_FALSE(again.first_drop);
        EXPECT_FALSE(limiter.Consume(source, 1000, 1).suppressed);
        break;
    }
    EXPECT_TRUE(found);
}

TEST(TurnAuthRateLimiterTest, InvalidInputsDoNotOccupyBuckets)
{
    TurnAuthRateLimiter limiter;
    EXPECT_TRUE(limiter.Consume({}, 0, 10).suppressed);
    EXPECT_TRUE(limiter.Consume(network::SocketAddr::FromIPPort("192.0.2.1", 1), 0, 0).suppressed);
    EXPECT_EQ(limiter.OccupiedBuckets(0), 0u);
}
} // namespace protocol
