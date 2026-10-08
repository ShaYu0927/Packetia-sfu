#include <gtest/gtest.h>

#include <limits>
#include "CryptoUtil.h"
#include "StringUtil.h"

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
