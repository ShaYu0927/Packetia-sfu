#include <gtest/gtest.h>

#include <limits>
#include <memory>
#include "BoundedPriorityQueue.h"
#include "CryptoUtil.h"
#include "StringUtil.h"

TEST(BoundedPriorityQueueTest, BucketsHaveIndependentLimitsAndPopHighFirstInFifoOrder)
{
    utils::BoundedPriorityQueue<int> queue(2, 2);
    using Priority = decltype(queue)::Priority;

    EXPECT_TRUE(queue.Empty());
    ASSERT_TRUE(queue.TryPush(Priority::Normal, 1));
    ASSERT_TRUE(queue.TryPush(Priority::Normal, 2));
    EXPECT_FALSE(queue.TryPush(Priority::Normal, 3));
    ASSERT_TRUE(queue.TryPush(Priority::High, 4));
    ASSERT_TRUE(queue.TryPush(Priority::High, 5));
    EXPECT_FALSE(queue.TryPush(Priority::High, 6));

    EXPECT_EQ(queue.TryPop(), 4);
    ASSERT_TRUE(queue.TryPush(Priority::High, 6));
    EXPECT_EQ(queue.TryPop(), 5);
    EXPECT_EQ(queue.TryPop(), 6);
    EXPECT_EQ(queue.TryPop(), 1);
    EXPECT_EQ(queue.TryPop(), 2);
    EXPECT_TRUE(queue.Empty());
    EXPECT_FALSE(queue.TryPop());
}

TEST(BoundedPriorityQueueTest, RejectedPushPreservesMoveOnlyValue)
{
    utils::BoundedPriorityQueue<std::unique_ptr<int>> queue(1, 0);
    using Priority = decltype(queue)::Priority;
    auto value = std::make_unique<int>(7);

    EXPECT_FALSE(queue.TryPush(Priority::High, std::move(value)));
    ASSERT_TRUE(value);
    ASSERT_TRUE(queue.TryPush(Priority::Normal, std::move(value)));
    EXPECT_FALSE(value);
    auto popped = queue.TryPop();
    ASSERT_TRUE(popped);
    ASSERT_TRUE(*popped);
    EXPECT_EQ(**popped, 7);
}

TEST(CryptoUtilTest, OversizedRandomRequestFailsBeforeAllocationAndClearsOutput)
{
    std::string out = "old-value";
    const size_t too_large = static_cast<size_t>(std::numeric_limits<int>::max()) + 1;
    EXPECT_FALSE(utils::SecureRandomHex(too_large, out));
    EXPECT_TRUE(out.empty());
}

TEST(CryptoUtilTest, ZeroLengthRequestReplacesPreviousOutputWithEmptyString)
{
    std::string out = "old-value";
    EXPECT_TRUE(utils::SecureRandomHex(0, out));
    EXPECT_TRUE(out.empty());
}

TEST(StringUtilTest, PrintableAsciiIncludesSpaceAndRejectsControlAndHighBytes)
{
    EXPECT_TRUE(utils::IsPrintableAscii({}));
    EXPECT_TRUE(utils::IsPrintableAscii(" space~"));
    for (const auto text : {std::string("a\0b", 3), std::string("a\nb"),
                            std::string(1, '\x7F'), std::string(1, '\xFF')})
        EXPECT_FALSE(utils::IsPrintableAscii(text));
}
