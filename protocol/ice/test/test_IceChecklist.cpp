#include <gtest/gtest.h>
#include "IceCandidate.h"
#include "IceChecklist.h"
namespace ice 
{


IceCandidate MakeCandidate(const std::string& foundation,
                           uint8_t component,
                           uint32_t priority,
                           const net::Endpoint& addr)
{

    IceCandidate c;
    c.SetFoundation(foundation);
    c.SetComponent(component);
    c.SetPriority(priority);
    c.SetAddress(addr);
    c.SetBaseAddress(addr);
    return c;
}

static net::Endpoint MakeV4(uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint16_t port)
{
    uint32_t ip_be =
        (static_cast<uint32_t>(a) << 24) |
        (static_cast<uint32_t>(b) << 16) |
        (static_cast<uint32_t>(c) << 8)  |
        static_cast<uint32_t>(d);

    return net::Endpoint::FromIPv4(ip_be, port);
}

TEST(IceChecklistTest, BuildPairsEmptyInput)
{
    IceChecklist checklist;

    std::vector<IceCandidate> locals;
    std::vector<IceCandidate> remotes;

    checklist.BuildPairs(locals, remotes);

    EXPECT_TRUE(checklist.pairs().empty());
}

TEST(IceChecklistTest, BuildPairsCartesianProduct)
{
    IceChecklist checklist;

    std::vector<IceCandidate> locals = {
        MakeCandidate("l1", 1, 200, MakeV4(10, 0, 0, 1, 5000)),
        MakeCandidate("l2", 1, 180, MakeV4(10, 0, 0, 1, 5001)),
    };

    std::vector<IceCandidate> remotes = {
        MakeCandidate("r1", 1, 210, MakeV4(20, 0, 0, 1, 6000)),
        MakeCandidate("r2", 1, 190, MakeV4(20, 0, 0, 1, 6001)),
        MakeCandidate("r3", 1, 170, MakeV4(20, 0, 0, 1, 6002)),
    };

    checklist.BuildPairs(locals, remotes);

    ASSERT_EQ(checklist.pairs().size(), 6u);
}

TEST(IceChecklistTest, BuildPairsPriorityAssigned)
{
    IceChecklist checklist;

    std::vector<IceCandidate> locals = 
    {
        MakeCandidate("l1", 1, 200, MakeV4(10, 0, 0, 1, 5000)),
    };

    std::vector<IceCandidate> remotes = {
        MakeCandidate("r1", 1, 210, MakeV4(20, 0, 0, 1, 6000)),
    };

    checklist.BuildPairs(locals, remotes);

    ASSERT_EQ(checklist.pairs().size(), 1u);
    EXPECT_NE(checklist.pairs()[0].priority, 0u);
}

TEST(IceChecklistTest, SortPairsDescendingByPriority)
{
    IceChecklist checklist;

    std::vector<IceCandidate> locals = {
        MakeCandidate("l1", 1, 100, MakeV4(10, 0, 0, 1, 5000)),
        MakeCandidate("l2", 1, 200, MakeV4(10, 0, 0, 1, 5001)),
    };

    std::vector<IceCandidate> remotes = {
        MakeCandidate("r1", 1, 150, MakeV4(20, 0, 0, 1, 6000)),
    };

    checklist.BuildPairs(locals, remotes);
    ASSERT_EQ(checklist.pairs().size(), 2u);

    checklist.SortPairs();

    EXPECT_GE(checklist.pairs()[0].priority, checklist.pairs()[1].priority);
    EXPECT_EQ(checklist.pairs()[0].local.Foundation(), "l2");
    EXPECT_EQ(checklist.pairs()[1].local.Foundation(), "l1");
}


}