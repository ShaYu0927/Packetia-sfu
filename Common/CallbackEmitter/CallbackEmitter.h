#ifndef _CALLBACKEMITTER_H_
#define _CALLBACKEMITTER_H_

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

struct ISubscription
{
    virtual ~ISubscription() = default;
    virtual void cancel() = 0;
};

template <class... Args>
struct ISignal
{
    using Callback = std::function<void(Args...)>;
    virtual ~ISignal() = default;

    virtual std::shared_ptr<ISubscription> subscribe(Callback cb) = 0;
    virtual void emit(Args... args) = 0;
    virtual size_t size() const = 0;
};

// Callbacks run synchronously on the emitting thread, outside the writer lock.
// Concurrent emits can enter the same callback concurrently. Cancellation does
// not wait for callbacks: one which passed its active check may still execute.
// Keep the signal alive for member calls; subscription handles may outlive it.
template <class... Args>
class SignalCOW : public ISignal<Args...>
{
public:
    using Callback = typename ISignal<Args...>::Callback;

    SignalCOW() : state_(std::make_shared<State>()) {}
    SignalCOW(const SignalCOW&) = delete;
    SignalCOW& operator=(const SignalCOW&) = delete;
    SignalCOW(SignalCOW&&) = delete;
    SignalCOW& operator=(SignalCOW&&) = delete;

    std::shared_ptr<ISubscription> subscribe(Callback cb) override
    {
        if (!cb) return nullptr;

        auto state = state_;
        auto entry = std::make_shared<Entry>(std::move(cb));
        // Allocate the handle before publication: allocation failure must not
        // leave a callback registered without a handle that can cancel it.
        auto subscription = std::make_shared<Subscription>(state, entry);
        state->add(entry);
        return subscription;
    }

    void emit(Args... args) override
    {
        auto snapshot = state_->load();
        for (const auto& entry : *snapshot)
        {
            if (entry->active.load(std::memory_order_acquire))
            {
                entry->callback(args...);
            }
        }
    }

    size_t size() const override
    {
        auto snapshot = state_->load();
        return std::count_if(snapshot->begin(), snapshot->end(), [](const auto& entry) {
            return entry->active.load(std::memory_order_acquire);
        });
    }

private:
    struct Entry
    {
        explicit Entry(Callback cb) : callback(std::move(cb)) {}
        Callback callback;
        std::atomic<bool> active{true};
    };

    using Entries = std::vector<std::shared_ptr<Entry>>;
    using Snapshot = std::shared_ptr<const Entries>;

    struct State
    {
        Snapshot load() const
        {
            return std::atomic_load_explicit(&snapshot, std::memory_order_acquire);
        }

        void add(const std::shared_ptr<Entry>& entry)
        {
            // Releasing an old callback can destroy captured subscriptions and
            // reenter this state. Retire snapshots only after unlocking.
            Snapshot previous;
            {
                std::lock_guard<std::mutex> lock(mutex);
                previous = load();
                auto next = std::make_shared<Entries>();
                next->reserve(previous->size() + 1);
                for (const auto& current : *previous)
                {
                    if (current->active.load(std::memory_order_acquire))
                        next->push_back(current);
                }
                next->push_back(entry);
                std::atomic_store_explicit(&snapshot, Snapshot(std::move(next)),
                                           std::memory_order_release);
            }
        }

        void remove(const std::shared_ptr<Entry>& entry)
        {
            Snapshot previous;
            {
                std::lock_guard<std::mutex> lock(mutex);
                previous = load();
                if (std::find(previous->begin(), previous->end(), entry) == previous->end())
                    return;

                auto next = std::make_shared<Entries>();
                next->reserve(previous->size());
                for (const auto& current : *previous)
                {
                    if (current->active.load(std::memory_order_acquire))
                        next->push_back(current);
                }
                std::atomic_store_explicit(&snapshot, Snapshot(std::move(next)),
                                           std::memory_order_release);
            }
        }

        std::mutex mutex;
        Snapshot snapshot = std::make_shared<const Entries>();
    };

    struct Subscription final : ISubscription
    {
        Subscription(const std::shared_ptr<State>& state, const std::shared_ptr<Entry>& entry)
            : state(state), entry(entry) {}

        ~Subscription() override { cancel(); }

        void cancel() noexcept override
        {
            auto current = entry.lock();
            if (!current || !current->active.exchange(false, std::memory_order_acq_rel))
                return;

            if (auto owner = state.lock())
            {
                try
                {
                    owner->remove(current);
                }
                catch (...)
                {
                    // Logical cancellation is already complete. If locking or
                    // copying fails, a later mutation prunes the inactive entry.
                }
            }
        }

        // Never mutate these weak pointers: concurrent cancel() calls only
        // coordinate through the entry's atomic flag.
        const std::weak_ptr<State> state;
        const std::weak_ptr<Entry> entry;
    };

    const std::shared_ptr<State> state_;
};

#endif /* _CALLBACKEMITTER_H_ */
