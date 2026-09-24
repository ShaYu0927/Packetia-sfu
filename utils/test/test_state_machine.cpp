#include "StateMachine.h"
#include "StateController.h"
#include <gtest/gtest.h>

namespace
{
enum class State { Idle = 10, Running = 40, Closed = 99 };
enum class Event { Start, Data, Close };
struct Context { bool ready = false; int total = 0; };
using Machine = utils::StateMachine<State, Event, Context, int>;

TEST(StateMachine, TransitionAndTerminalState)
{
    Machine machine(State::Idle, {{State::Idle, Event::Start, State::Running},
                                 {State::Running, Event::Close, State::Closed}});
    Context context;
    auto result = machine.Process(Event::Start, context, 0);
    EXPECT_TRUE(result.Accepted());
    EXPECT_TRUE(result.Changed());
    EXPECT_EQ(result.before, State::Idle);
    EXPECT_EQ(result.event, Event::Start);
    EXPECT_EQ(result.after, State::Running);
    EXPECT_TRUE(machine.Process(Event::Close, context, 0).Changed());
    EXPECT_EQ(machine.Process(Event::Start, context, 0).status, Machine::Status::Unhandled);
    EXPECT_EQ(machine.CurrentState(), State::Closed);
}

TEST(StateMachine, GuardAndSelfTransition)
{
    Machine machine(State::Idle, {
        {State::Idle, Event::Start, State::Running,
         [](const Context& c, const int&) { return c.ready; }},
        {State::Running, Event::Data, State::Running, {},
         [](Context& c, const int& value) { c.total += value; }}
    });
    Context context;
    EXPECT_EQ(machine.Process(Event::Start, context, 0).status, Machine::Status::GuardRejected);
    EXPECT_EQ(machine.CurrentState(), State::Idle);
    context.ready = true;
    EXPECT_TRUE(machine.Process(Event::Start, context, 0).Changed());
    auto result = machine.Process(Event::Data, context, 7);
    EXPECT_TRUE(result.Accepted());
    EXPECT_FALSE(result.Changed());
    EXPECT_EQ(context.total, 7);
    EXPECT_EQ(machine.Process(Event::Start, context, 0).status, Machine::Status::Unhandled);
}

TEST(StateMachine, DuplicateRulesFailAtConstruction)
{
    EXPECT_THROW((Machine(State::Idle, {
        {State::Idle, Event::Start, State::Running},
        {State::Idle, Event::Start, State::Closed}})), std::invalid_argument);
}

TEST(StateMachine, EmptyTableAndUnknownEvent)
{
    Machine machine(State::Idle, {});
    Context context;
    EXPECT_EQ(machine.Process(static_cast<Event>(999), context, 0).status, Machine::Status::Unhandled);
    EXPECT_EQ(machine.CurrentState(), State::Idle);
}

TEST(StateMachine, ActionObservesCommittedStateAndRejectsReentry)
{
    Machine* owner = nullptr;
    Machine machine(State::Idle, {
        {State::Idle, Event::Start, State::Running, {},
         [&](Context& c, const int& value) {
             EXPECT_EQ(owner->CurrentState(), State::Running);
             EXPECT_EQ(owner->Process(Event::Close, c, value).status, Machine::Status::Busy);
         }},
        {State::Running, Event::Close, State::Closed}
    });
    owner = &machine;
    Context context;
    EXPECT_TRUE(machine.Process(Event::Start, context, 0).Changed());
    EXPECT_TRUE(machine.Process(Event::Close, context, 0).Changed());
}

TEST(StateMachine, ActionExceptionKeepsCommitAndReleasesProcessingFlag)
{
    Machine machine(State::Idle, {
        {State::Idle, Event::Start, State::Running, {},
         [](Context&, const int&) { throw std::runtime_error("action failed"); }},
        {State::Running, Event::Close, State::Closed}
    });
    Context context;
    EXPECT_THROW(machine.Process(Event::Start, context, 0), std::runtime_error);
    EXPECT_EQ(machine.CurrentState(), State::Running);
    EXPECT_TRUE(machine.Process(Event::Close, context, 0).Changed());
}

TEST(StateMachine, GuardExceptionDoesNotCommitAndCanRetry)
{
    Machine machine(State::Idle, {
        {State::Idle, Event::Start, State::Running,
         [](const Context& c, const int&) {
             if (!c.ready) throw std::runtime_error("guard failed");
             return true;
         }}
    });
    Context context;
    EXPECT_THROW(machine.Process(Event::Start, context, 0), std::runtime_error);
    EXPECT_EQ(machine.CurrentState(), State::Idle);
    context.ready = true;
    EXPECT_TRUE(machine.Process(Event::Start, context, 0).Changed());
}

TEST(StateMachine, InstancesAndTypedPayloadAreIndependent)
{
    struct Payload { int bytes; };
    using Other = utils::StateMachine<State, Event, Context, Payload>;
    auto action = [](Context& c, const Payload& p) { c.total += p.bytes; };
    Other first(State::Idle, {{State::Idle, Event::Start, State::Running, {}, action}});
    Other second(State::Idle, {{State::Idle, Event::Start, State::Running, {}, action}});
    Context a, b;
    EXPECT_TRUE(first.Process(Event::Start, a, {42}).Accepted());
    EXPECT_EQ(a.total, 42);
    EXPECT_EQ(b.total, 0);
    EXPECT_EQ(second.CurrentState(), State::Idle);
}

TEST(StateController, BoundContextAndDefaultEventPayload)
{
    using Controller = utils::StateController<State, Event, Context>;
    Context context;
    Controller controller(context, State::Idle, {
        Controller::On(State::Idle, Event::Start, State::Running,
            [](Context& c, const Event& event) {
                EXPECT_EQ(event, Event::Start);
                ++c.total;
            }),
        Controller::Stay(State::Running, Event::Data),
        Controller::On(State::Running, Event::Close, State::Closed)
    });
    EXPECT_TRUE(controller.Dispatch(Event::Start).Changed());
    EXPECT_EQ(context.total, 1);
    EXPECT_TRUE(controller.Is(State::Running));
    auto ignored = controller.Dispatch(Event::Data);
    EXPECT_TRUE(ignored.Accepted());
    EXPECT_FALSE(ignored.Changed());
    EXPECT_TRUE(controller.Dispatch(Event::Close).Changed());
    EXPECT_EQ(controller.Dispatch(Event::Start).status, Controller::Status::Unhandled);
}

TEST(StateController, GuardAndExplicitOrDefaultPayload)
{
    using Controller = utils::StateController<State, Event, Context, int>;
    Context context;
    Controller controller(context, State::Idle, {
        Controller::On(State::Idle, Event::Start, State::Running,
            [](Context& c, const int& count) { c.total += count; },
            [](const Context& c, const int&) { return c.ready; }),
        Controller::Stay(State::Running, Event::Data,
            [](Context& c, const int& count) { c.total += count; })
    });
    EXPECT_EQ(controller.Dispatch(Event::Start, 5).status, Controller::Status::GuardRejected);
    EXPECT_EQ(context.total, 0);
    context.ready = true;
    EXPECT_TRUE(controller.Dispatch(Event::Start, 5).Changed());
    EXPECT_TRUE(controller.Dispatch(Event::Data).Accepted());
    EXPECT_EQ(context.total, 5);
}

TEST(StateController, NonDefaultConstructiblePayload)
{
    struct Input { explicit Input(int value) : value(value) {} int value; };
    using Controller = utils::StateController<State, Event, Context, Input>;
    Context context;
    Controller controller(context, State::Idle, {
        Controller::On(State::Idle, Event::Start, State::Running,
            [](Context& c, const Input& input) { c.total = input.value; })
    });
    EXPECT_TRUE(controller.Dispatch(Event::Start, Input{42}).Accepted());
    EXPECT_EQ(context.total, 42);
}

TEST(StateController, ReentryAndExceptionKeepCoreSemantics)
{
    using Controller = utils::StateController<State, Event, Context>;
    Context context;
    Controller* owner = nullptr;
    Controller controller(context, State::Idle, {
        Controller::On(State::Idle, Event::Start, State::Running,
            [&](Context&, const Event&) {
                EXPECT_TRUE(owner->Is(State::Running));
                EXPECT_EQ(owner->Dispatch(Event::Close).status, Controller::Status::Busy);
                throw std::runtime_error("failed action");
            }),
        Controller::On(State::Running, Event::Close, State::Closed)
    });
    owner = &controller;
    EXPECT_THROW((void)controller.Dispatch(Event::Start), std::runtime_error);
    EXPECT_TRUE(controller.Is(State::Running));
    EXPECT_TRUE(controller.Dispatch(Event::Close).Changed());
}
} // namespace
