#include "signaling/SignalSession.h"
#include <gtest/gtest.h>
#include <deque>
#include <thread>
#include <vector>

namespace
{
using namespace server::signaling;
class Executor
{
public:
    bool Post(std::function<void()> task)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_.push_back(std::move(task)); return true;
    }
    void Run()
    {
        for (;;)
        {
            std::function<void()> task;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (tasks_.empty()) return;
                task = std::move(tasks_.front()); tasks_.pop_front();
            }
            task();
        }
    }
private:
    std::mutex mutex_;
    std::deque<std::function<void()>> tasks_;
};
TEST(SignalChannel, FifoAndBoundedInitialNotificationGate)
{
    Executor executor;
    auto budget = std::make_shared<SignalBudget>(8, 4);
    std::vector<int> received;
    SignalChannel<int> channel([&](auto task) { return executor.Post(std::move(task)); },
        [&](int value) { received.push_back(value); }, budget, 8, 4, true);
    EXPECT_EQ(channel.Send(2, 2, true), SignalResult::Accepted);
    EXPECT_EQ(channel.Send(3, 2, true), SignalResult::Accepted);
    executor.Run();
    EXPECT_TRUE(received.empty());
    EXPECT_EQ(channel.Send(9, 5, true), SignalResult::Full);
    EXPECT_EQ(channel.Send(1, 2, false, true), SignalResult::Accepted);
    executor.Run();
    EXPECT_EQ(received, (std::vector<int>{1, 2, 3}));
    EXPECT_EQ(budget->Used(), (std::make_pair(std::size_t{0}, std::size_t{0})));
    channel.ResetNotificationGate();
    EXPECT_EQ(channel.Send(5, 1, true), SignalResult::Accepted);
    EXPECT_EQ(channel.Send(4, 1, false, true), SignalResult::Accepted);
    executor.Run();
    EXPECT_EQ(received, (std::vector<int>{1, 2, 3, 4, 5}));
}
TEST(SignalChannel, InFlightAndBothDirectionsShareBudget)
{
    Executor executor;
    auto budget = std::make_shared<SignalBudget>(4, 1);
    auto post = [&](auto task) { return executor.Post(std::move(task)); };
    SignalChannel<int> second(post, [](int) {}, budget);
    SignalChannel<int>* current = nullptr;
    SignalChannel<int> first(post, [&](int) {
        EXPECT_EQ(budget->Used(), (std::make_pair(std::size_t{4}, std::size_t{1})));
        EXPECT_EQ(second.Send(2, 1), SignalResult::Full);
        current->Cancel();
        EXPECT_EQ(budget->Used().second, 1u);
    }, budget);
    current = &first;
    ASSERT_EQ(first.Send(1, 4), SignalResult::Accepted);
    executor.Run();
    EXPECT_EQ(budget->Used().second, 0u);
    EXPECT_EQ(first.Send(1, 1), SignalResult::Cancelled);
    EXPECT_EQ(second.Send(2, 4), SignalResult::Accepted);
    executor.Run();
    EXPECT_EQ(budget->Used().second, 0u);
}
TEST(SignalChannel, CloseCancellationAndRejectedExecutorReleaseReservations)
{
    Executor executor;
    auto budget = std::make_shared<SignalBudget>();
    int received = 0;
    {
        SignalChannel<int> channel([&](auto task) { return executor.Post(std::move(task)); },
            [&](int) { ++received; }, budget);
        ASSERT_EQ(channel.Send(1, 10), SignalResult::Accepted);
        channel.Close(); channel.Close(); channel.Cancel();
        EXPECT_EQ(channel.Send(2, 1), SignalResult::Closed);
        EXPECT_EQ(budget->Used().second, 0u);
    }
    executor.Run();
    EXPECT_EQ(received, 0);
    SignalChannel<int> rejected([](auto) { return false; }, [](int) {}, budget);
    EXPECT_EQ(rejected.Send(1, 10), SignalResult::Cancelled);
    EXPECT_EQ(budget->Used().second, 0u);
    SignalChannel<int> throwing([&](auto task) { return executor.Post(std::move(task)); },
        [](int) { throw std::runtime_error("consumer failed"); }, budget);
    ASSERT_EQ(throwing.Send(1, 10), SignalResult::Accepted);
    ASSERT_EQ(throwing.Send(2, 10), SignalResult::Accepted);
    executor.Run();
    EXPECT_EQ(throwing.Status(), SignalResult::Cancelled);
    EXPECT_EQ(budget->Used().second, 0u);
}
TEST(SignalChannel, MessageCountAndProducerOrdering)
{
    Executor executor;
    auto budget = std::make_shared<SignalBudget>();
    std::vector<std::pair<int, int>> received;
    SignalChannel<std::pair<int, int>> channel([&](auto task) { return executor.Post(std::move(task)); },
        [&](auto value) { received.push_back(value); }, budget, 4096, 64);
    std::atomic<int> accepted{0}, full{0};
    std::vector<std::thread> producers;
    for (int producer = 0; producer < 4; ++producer)
        producers.emplace_back([&, producer] {
            for (int sequence = 0; sequence < 32; ++sequence)
            {
                const auto result = channel.Send({producer, sequence}, 1);
                if (result == SignalResult::Accepted) ++accepted;
                if (result == SignalResult::Full) ++full;
            }
        });
    for (auto& producer : producers) producer.join();
    EXPECT_EQ(accepted, 64); EXPECT_EQ(full, 64);
    executor.Run();
    EXPECT_EQ(received.size(), 64u);
    std::vector<int> last(4, -1);
    for (const auto& value : received)
    { EXPECT_GT(value.second, last[value.first]); last[value.first] = value.second; }
    EXPECT_EQ(budget->Used().second, 0u);
}
TEST(SignalSession, DistinctExecutorsCancelAndFinalResponse)
{
    Executor room, connection;
    auto budget = std::make_shared<SignalBudget>();
    std::vector<SignalResponse> received;
    std::unique_ptr<SignalSession> session;
    session = std::make_unique<SignalSession>(SignalKey{"WS_test", 42},
        [&](auto task) { return room.Post(std::move(task)); },
        [&](auto task) { return connection.Post(std::move(task)); },
        [&](auto) { session->Respond(Json{{"type", "joined"}}, true); },
        [&](auto response) { received.push_back(std::move(response)); }, budget);
    EXPECT_EQ(session->Respond(Json{{"type", "notice"}}, false, true), SignalResult::Accepted);
    EXPECT_EQ(session->Request({SignalRequest::Type::Join, Json::object()}, 10), SignalResult::Accepted);
    connection.Run(); EXPECT_TRUE(received.empty());
    room.Run(); EXPECT_TRUE(received.empty());
    connection.Run(); ASSERT_EQ(received.size(), 2u);
    EXPECT_EQ(received[0].body["type"], "joined");
    EXPECT_EQ(received[1].body["type"], "notice");
    EXPECT_EQ(session->Finish(Error("done")), SignalResult::Accepted);
    EXPECT_EQ(session->Request({SignalRequest::Type::Leave, {}}, 10), SignalResult::Closed);
    EXPECT_EQ(session->Finish(Error("again")), SignalResult::Closed);
    connection.Run(); ASSERT_EQ(received.size(), 3u);
    EXPECT_TRUE(received.back().close_after);
    session->Cancel(); session->Cancel();
    EXPECT_TRUE(session->IsCancelled());
    EXPECT_EQ(budget->Used().second, 0u);
}
TEST(SignalSession, QueueOverflowCancelsBothDirections)
{
    Executor room, connection;
    auto budget = std::make_shared<SignalBudget>();
    int received = 0;
    SignalSession session({"overflow", 1}, [&](auto task) { return room.Post(std::move(task)); },
        [&](auto task) { return connection.Post(std::move(task)); },
        [&](auto) { ++received; }, [&](auto) { ++received; }, budget);
    for (int i = 0; i < 64; ++i)
        ASSERT_EQ(session.Request({SignalRequest::Type::Tracks, {}}, 1), SignalResult::Accepted);
    EXPECT_EQ(session.Request({SignalRequest::Type::Tracks, {}}, 1), SignalResult::Full);
    EXPECT_TRUE(session.IsCancelled());
    room.Run(); connection.Run();
    EXPECT_EQ(received, 0); EXPECT_EQ(budget->Used().second, 0u);
}
}
