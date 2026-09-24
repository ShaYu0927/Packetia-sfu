#pragma once

#include "StateMachine.h"
#include <utility>

namespace utils
{
// Convenience facade. Context is borrowed and must outlive this controller.
// Same serialization, reentry and exception contract as StateMachine.
template<class State, class Event, class Context, class Payload = Event>
class StateController
{
public:
    using Machine = StateMachine<State, Event, Context, Payload>;
    using Transition = typename Machine::Transition;
    using Action = typename Machine::Action;
    using Guard = typename Machine::Guard;
    using Result = typename Machine::Result;
    using Status = typename Machine::Status;

    StateController(Context& context, State initial,
                    std::initializer_list<Transition> transitions)
        : context_(context), machine_(initial, transitions) {}

    // Action comes before optional guard for the common unguarded case.
    static Transition On(State from, Event event, State to,
                         Action action = {}, Guard guard = {})
    {
        return {from, event, to, std::move(guard), std::move(action)};
    }

    // Handle an event without changing state; no action means explicitly ignore.
    static Transition Stay(State state, Event event, Action action = {}, Guard guard = {})
    {
        return On(state, event, state, std::move(action), std::move(guard));
    }

    State CurrentState() const noexcept { return machine_.CurrentState(); }
    bool Is(State state) const noexcept { return CurrentState() == state; }

    [[nodiscard]] Result Dispatch(Event event, const Payload& payload)
    {
        return machine_.Process(event, context_, payload);
    }

    // With the default Payload=Event, callbacks receive the actual event.
    // Custom payloads may be omitted only if they are default-constructible.
    template<class P = Payload,
             std::enable_if_t<std::is_same_v<P, Payload> &&
                              std::is_default_constructible_v<P>, int> = 0>
    [[nodiscard]] Result Dispatch(Event event)
    {
        if constexpr (std::is_same_v<Payload, Event>) return Dispatch(event, event);
        else return Dispatch(event, Payload{});
    }

private:
    Context& context_;
    Machine machine_;
};
} // namespace utils
