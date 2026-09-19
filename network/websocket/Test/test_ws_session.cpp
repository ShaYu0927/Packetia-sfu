#include "websocket/WsSession.h"
#include "websocket/WsSessionManager.h"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <utility>
#include <vector>

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #condition << '\n'; \
    std::abort(); } } while (false)

namespace
{
using namespace network;

WsLimits SmallLimits()
{
    WsLimits limits;
    limits.max_message_bytes = 8;
    limits.max_queued_bytes = 12;
    limits.max_queued_messages = 3;
    limits.max_total_queued_bytes = 24;
    limits.max_total_queued_messages = 6;
    return limits;
}

void CheckStats(const WsSendStats& stats, std::size_t bytes, std::size_t messages)
{
    CHECK(stats.bytes == bytes);
    CHECK(stats.messages == messages);
}

void TestInflightReservationAndMove()
{
    auto limits = SmallLimits();
    auto budget = std::make_shared<WsSendBudget>(24, 6);
    WsSession session("one", {}, limits, budget);
    CHECK(!session.SendText("before open"));
    session.OnOpen();
    CHECK(!session.SendText("123456789"));
    CHECK(session.SendText("12345678"));
    CHECK(session.SendText("1234"));
    CHECK(!session.SendText("x"));
    WsOutgoingMessage first;
    CHECK(session.PopOutgoing(first));
    CHECK(first.Data() == "12345678");
    CheckStats(budget->GetStats(), 12, 2);
    CHECK(!session.SendText("x"));

    WsOutgoingMessage moved(std::move(first));
    CHECK(first.Empty());
    CHECK(!moved.Empty());
    first.Reset();
    CheckStats(session.GetSendStats(), 12, 2);
    WsOutgoingMessage second;
    CHECK(session.PopOutgoing(second));
    second = std::move(moved); // Return the old four-byte reservation.
    CheckStats(budget->GetStats(), 8, 1);
    CHECK(moved.Empty());
    second.Reset();
    second.Reset();
    CheckStats(budget->GetStats(), 0, 0);
}

void TestEmptyFramesAndMessageLimit()
{
    auto limits = SmallLimits();
    WsSession session("empty", {}, limits);
    session.OnOpen();
    CHECK(session.SendText(""));
    CHECK(session.SendText(""));
    CHECK(session.SendText(""));
    CHECK(!session.SendText(""));
    WsOutgoingMessage message;
    CHECK(session.PopOutgoing(message));
    CHECK(!message.Empty());
    CHECK(message.Data().empty());
    CHECK(!session.SendText("")); // In-flight empty frames count as messages.
    message.Reset();
    CHECK(session.SendText(""));
    CheckStats(session.GetSendStats(), 0, 3);
    session.Close();
    CheckStats(session.GetSendStats(), 0, 0);

    // Byte limits alone cannot bound a flood of empty frames across sessions.
    auto budget = std::make_shared<WsSendBudget>(100, 2);
    WsSession one("empty-one", {}, limits, budget), two("empty-two", {}, limits, budget);
    one.OnOpen();
    two.OnOpen();
    CHECK(one.SendText(""));
    CHECK(two.SendText(""));
    CHECK(!one.SendText(""));
    CHECK(!two.SendText(""));
    CHECK(one.PopOutgoing(message));
    CHECK(!two.SendText(""));
    message.Reset();
    CHECK(two.SendText(""));
    CheckStats(budget->GetStats(), 0, 2);
}

void TestCloseReturnsOnlyItsOwnReservations()
{
    auto limits = SmallLimits();
    auto budget = std::make_shared<WsSendBudget>(12, 3);
    WsSession one("one", {}, limits, budget), two("two", {}, limits, budget);
    one.OnOpen();
    two.OnOpen();
    CHECK(one.SendText("12345678"));
    CHECK(two.SendText("1234"));
    CHECK(!two.SendText("x"));
    WsOutgoingMessage inflight;
    CHECK(one.PopOutgoing(inflight));
    one.Close();
    CheckStats(budget->GetStats(), 4, 1);
    CHECK(two.SendText("12345678"));
    inflight.Reset(); // Must not subtract again after Close.
    one.OnClosed();
    one.Close();
    CheckStats(budget->GetStats(), 12, 2);
    CHECK(!one.SendText(""));
    CHECK(!one.PopOutgoing(inflight));
    two.OnClosed();
    CheckStats(budget->GetStats(), 0, 0);
}

void TestDestructionReturnsInflightReservation()
{
    auto limits = SmallLimits();
    limits.max_message_bytes = 4096;
    limits.max_queued_bytes = 4096;
    auto budget = std::make_shared<WsSendBudget>(4096, 3);
    WsOutgoingMessage inflight;
    {
        WsSession session("temporary", {}, limits, budget);
        session.OnOpen();
        CHECK(session.SendText(std::string(2048, 'x')));
        CHECK(session.PopOutgoing(inflight));
        CheckStats(budget->GetStats(), 2048, 1);
    }
    CheckStats(budget->GetStats(), 0, 0);
    inflight.Reset();
    CHECK(inflight.Data().capacity() <= std::string{}.capacity());
    CheckStats(budget->GetStats(), 0, 0);
}

void TestWakeTransitionsAndReentrancy()
{
    int wakes = 0;
    WsSession* current = nullptr;
    WsSession session("wake", [&] {
        ++wakes;
        // These calls take the session mutex and would deadlock if wake ran
        // under it. Reentrant enqueue must not recursively wake a nonempty queue.
        CHECK(current->NeedsWritable());
        if (!current->IsClosing() && wakes == 1) CHECK(current->SendText("b"));
    }, SmallLimits());
    current = &session;
    session.OnOpen();
    CHECK(session.SendText("a"));
    CHECK(wakes == 1);
    WsOutgoingMessage message;
    CHECK(session.PopOutgoing(message));
    CHECK(message.Data() == "a");
    CHECK(session.PopOutgoing(message));
    CHECK(message.Data() == "b");
    message.Reset();
    CHECK(session.SendText("c"));
    CHECK(wakes == 2);
    session.Close();
    CHECK(wakes == 3);
    session.Close();
    session.OnClosed();
    CHECK(wakes == 3);
}

void TestFragmentBoundariesAndReentrantCallback()
{
    WsSession session("fragments", {}, SmallLimits());
    session.OnOpen();
    std::vector<std::string> messages;
    session.SetOnMessage([&](const std::string& id, const std::string& data) {
        CHECK(id == "fragments");
        messages.push_back(data);
        CHECK(session.SendText(data));
    });
    CHECK(session.ReceiveFragment("1234", 4, false));
    CHECK(messages.empty());
    CHECK(session.ReceiveFragment("5678", 4, true));
    CHECK(messages.size() == 1 && messages.front() == "12345678");
    CHECK(session.ReceiveFragment(nullptr, 0, true));
    CHECK(messages.size() == 2 && messages.back().empty());
    CHECK(!session.ReceiveFragment(nullptr, 1, true));
    CHECK(session.ReceiveFragment("12345678", 8, false));
    CHECK(!session.ReceiveFragment("9", 1, true));
    CHECK(messages.size() == 2);
    session.Close();
    CHECK(!session.ReceiveFragment("x", 1, true));
}

void TestConcurrentGlobalBudget()
{
    auto limits = SmallLimits();
    limits.max_queued_bytes = 4096;
    limits.max_queued_messages = 1024;
    auto budget = std::make_shared<WsSendBudget>(256, 64);
    std::vector<WsSession::Ptr> sessions;
    for (int i = 0; i != 8; ++i)
    {
        sessions.push_back(std::make_shared<WsSession>(std::to_string(i), WsSession::WakeCallback{}, limits, budget));
        sessions.back()->OnOpen();
    }
    std::atomic<bool> start{false};
    std::atomic<int> accepted{0};
    std::vector<std::thread> threads;
    for (auto& session : sessions)
        threads.emplace_back([&, session] {
            while (!start.load()) std::this_thread::yield();
            for (int i = 0; i != 128; ++i)
                if (session->SendText("data")) ++accepted;
        });
    start = true;
    for (auto& thread : threads) thread.join();
    CHECK(accepted == 64);
    CheckStats(budget->GetStats(), 256, 64);
    std::vector<WsOutgoingMessage> inflight;
    for (auto& session : sessions)
    {
        WsOutgoingMessage message;
        while (session->PopOutgoing(message)) inflight.push_back(std::move(message));
        CHECK(!session->SendText("x"));
    }
    CheckStats(budget->GetStats(), 256, 64);
    for (auto& session : sessions) session->Close();
    CheckStats(budget->GetStats(), 0, 0);
    inflight.clear();
    CheckStats(budget->GetStats(), 0, 0);
}

void TestConcurrentCloseAndDrain()
{
    auto budget = std::make_shared<WsSendBudget>(24, 6);
    WsSession session("race", {}, SmallLimits(), budget);
    session.OnOpen();
    std::atomic<int> attempts{0};
    std::atomic<bool> start{false};
    std::vector<std::thread> senders;
    for (int i = 0; i != 4; ++i)
        senders.emplace_back([&] {
            while (!start.load()) std::this_thread::yield();
            for (int j = 0; j != 2000; ++j)
            {
                session.SendText("data");
                ++attempts;
            }
        });
    std::thread consumer([&] {
        while (!start.load()) std::this_thread::yield();
        while (!session.IsClosing())
        {
            WsOutgoingMessage message;
            session.PopOutgoing(message);
            const auto stats = budget->GetStats();
            CHECK(stats.bytes <= 12 && stats.messages <= 3);
        }
    });
    start = true;
    while (attempts.load() < 1000) std::this_thread::yield();
    session.Close();
    for (auto& sender : senders) sender.join();
    consumer.join();
    CHECK(!session.SendText(""));
    CheckStats(budget->GetStats(), 0, 0);
}

void TestManagerLimitAndReentrantRemoval()
{
    WsSessionManager manager;
    std::atomic<bool> start{false};
    std::atomic<int> accepted{0};
    std::vector<std::thread> threads;
    for (int i = 0; i != 8; ++i)
        threads.emplace_back([&, i] {
            while (!start.load()) std::this_thread::yield();
            for (int j = 0; j != 32; ++j)
            {
                auto id = std::to_string(i) + "_" + std::to_string(j);
                auto session = std::make_shared<WsSession>(id, WsSession::WakeCallback{});
                if (manager.AddSession(session, 13))
                {
                    ++accepted;
                    CHECK(manager.GetSession(id) == session);
                }
                CHECK(manager.GetSessionCount() <= 13);
            }
        });
    start = true;
    for (auto& thread : threads) thread.join();
    CHECK(accepted == 13);
    CHECK(manager.GetSessionCount() == 13);
    CHECK(!manager.AddSession(nullptr, 13));

    WsSessionManager removal;
    int wakes = 0;
    auto session = std::make_shared<WsSession>("remove", [&] {
        ++wakes;
        CHECK(removal.GetSessionCount() == 0);
        CHECK(!removal.GetSession("remove"));
    });
    session->OnOpen();
    CHECK(removal.AddSession(session, 1));
    CHECK(!removal.AddSession(session, 1));
    removal.RemoveSession("remove");
    removal.RemoveSession("remove");
    CHECK(wakes == 1);
    CHECK(session->IsClosing());
}
}

int main()
{
    TestInflightReservationAndMove();
    TestEmptyFramesAndMessageLimit();
    TestCloseReturnsOnlyItsOwnReservations();
    TestDestructionReturnsInflightReservation();
    TestWakeTransitionsAndReentrancy();
    TestFragmentBoundariesAndReentrantCallback();
    TestConcurrentGlobalBudget();
    TestConcurrentCloseAndDrain();
    TestManagerLimitAndReentrantRemoval();
    std::cout << "PASS: 9 WebSocket session tests\n";
}
