#ifndef _CALLBACKEMITTER_H_
#define _CALLBACKEMITTER_H_

#include <functional>
#include <memory>
#include <mutex>
#include <atomic>
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

template <class... Args>
class SignalCOW : public ISignal<Args...>
{
public:
    using Callback = typename ISignal<Args...>::Callback;

    SignalCOW()
    {
        auto empty = std::make_shared<std::vector<Entry>>();
        std::atomic_store_explicit(&snapshot_, empty, std::memory_order_release);
    }

    std::shared_ptr<ISubscription> subscribe(Callback cb) override
    {
        if (!cb) return nullptr;

        const uint64_t id = next_id_.fetch_add(1, std::memory_order_relaxed) + 1;

        {
            std::lock_guard<std::mutex> lock(mtx_);
            auto cur = std::atomic_load_explicit(&snapshot_, std::memory_order_acquire);
            auto next = std::make_shared<std::vector<Entry>>(*cur);
            next->push_back(Entry{id, std::move(cb)});
            std::atomic_store_explicit(&snapshot_, next, std::memory_order_release);
        }

        struct Sub final : ISubscription
        {
            Sub(SignalCOW* owner, uint64_t id) : owner(owner), id(id) {}
            void cancel() override
            {
                auto* o = owner;
                owner = nullptr;
                if (o) o->unsubscribe(id);
            }
            ~Sub() override { cancel(); }
            SignalCOW* owner = nullptr;
            uint64_t id = 0;
        };

        return std::make_shared<Sub>(this, id);
    }

    void emit(Args... args) override
    {
        auto snap = std::atomic_load_explicit(&snapshot_, std::memory_order_acquire);
        for (auto& e : *snap)
        {
            e.cb(args...);
        }
    }

    size_t size() const override
    {
        auto snap = std::atomic_load_explicit(&snapshot_, std::memory_order_acquire);
        return snap->size();
    }

private:
    void unsubscribe(uint64_t id)
    {
        std::lock_guard<std::mutex> lock(mtx_);
        auto cur = std::atomic_load_explicit(&snapshot_, std::memory_order_acquire);
        if (cur->empty()) return;

        auto next = std::make_shared<std::vector<Entry>>();
        next->reserve(cur->size());

        bool changed = false;
        for (auto& e : *cur)
        {
            if (e.id == id) { changed = true; continue; }
            next->push_back(e);
        }
        if (changed)
        {
            std::atomic_store_explicit(&snapshot_, next, std::memory_order_release);
        }
    }

private:
    struct Entry { uint64_t id; Callback cb; };

    mutable std::mutex mtx_;
    mutable std::shared_ptr<std::vector<Entry>> snapshot_;
    std::atomic<uint64_t> next_id_{0};
};

#endif /* _CALLBACKEMITTER_H_ */