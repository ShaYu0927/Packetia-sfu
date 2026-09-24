#pragma once

#include <functional>
#include <initializer_list>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace utils
{
// Synchronous, owner-serialized state machine. Does not own Context or Payload.
// No internal threads, timers, event queues or locks.
template<class State, class Event, class Context, class Payload = Event>
class StateMachine
{
    static_assert(std::is_enum_v<State> && std::is_enum_v<Event>,
                  "State and Event must be enum types");
public:
    using Guard = std::function<bool(const Context&, const Payload&)>;
    using Action = std::function<void(Context&, const Payload&)>;

    struct Transition
    {
        State from;
        Event event;
        State to;
        Guard guard{};
        Action action{};
    };

    enum class Status { Applied, Unhandled, GuardRejected, Busy };
    struct Result
    {
        State before;
        Event event;
        State after;
        Status status;
        bool Accepted() const noexcept { return status == Status::Applied; }
        bool Changed() const noexcept { return Accepted() && before != after; }
    };

    // Own the table; temporary initializer lists and captured callbacks are safe.
    // Enum values need not be contiguous. No outgoing rows means a terminal state.
    StateMachine(State initial, std::initializer_list<Transition> transitions)
        : state_(initial), transitions_(transitions)
    {
        for (std::size_t i = 0; i < transitions_.size(); ++i)
            for (std::size_t j = 0; j < i; ++j)
                if (transitions_[i].from == transitions_[j].from &&
                    transitions_[i].event == transitions_[j].event)
                    throw std::invalid_argument("Duplicate state/event transition");
    }

    StateMachine(const StateMachine&) = delete;
    StateMachine& operator=(const StateMachine&) = delete;
    StateMachine(StateMachine&&) = delete;
    StateMachine& operator=(StateMachine&&) = delete;

    State CurrentState() const noexcept { return state_; }

    Result Process(Event event, Context& context, const Payload& payload)
    {
        const State before = state_;
        if (processing_) return {before, event, before, Status::Busy};
        struct ProcessingScope
        {
            bool& flag;
            explicit ProcessingScope(bool& value) : flag(value) { flag = true; }
            ~ProcessingScope() { flag = false; }
        } scope(processing_);

        for (const auto& transition : transitions_)
        {
            if (transition.from != before || transition.event != event) continue;
            if (transition.guard && !transition.guard(context, payload))
                return {before, event, before, Status::GuardRejected};

            // Commit before action: callbacks observe the new state. If action
            // throws, propagate; state stays committed (side effects cannot be
            // rolled back). Guard exceptions leave the state unchanged.
            state_ = transition.to;
            if (transition.action) transition.action(context, payload);
            return {before, event, state_, Status::Applied};
        }
        return {before, event, before, Status::Unhandled};
    }

private:
    State state_;
    const std::vector<Transition> transitions_;
    bool processing_{false};
};
} // namespace utils
