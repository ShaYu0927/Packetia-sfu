#ifndef _EVENT_BUS_H_
#define _EVENT_BUS_H_

#include "MediaEvent.h"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace media
{
// 同步分发；线程切换由订阅者负责。不同 Publish 调用之间不保证顺序。
class EventBus
{
public:
    using RawCallback = std::function<void(const MediaEvent&)>;
    struct DispatchResult
    {
        std::size_t delivered = 0;
        std::size_t failed = 0;
    };

private:
    struct Slot
    {
        explicit Slot(RawCallback cb) : callback(std::move(cb)) {}
        std::atomic<bool> active{true};
        RawCallback callback;
    };
    struct State
    {
        std::mutex mutex;
        bool closed = false;
        std::unordered_map<MediaEventType, std::vector<std::shared_ptr<Slot>>> handlers;
    };

public:
    class Subscription
    {
    public:
        Subscription() = default;
        ~Subscription() { Reset(); }
        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;
        Subscription(Subscription&& other) noexcept
            : state_(std::move(other.state_)), slot_(std::move(other.slot_)), type_(other.type_) {}
        Subscription& operator=(Subscription&& other) noexcept
        {
            if (this != &other)
            {
                Reset();
                state_ = std::move(other.state_);
                slot_ = std::move(other.slot_);
                type_ = other.type_;
            }
            return *this;
        }
        void Reset()
        {
            const auto slot = slot_.lock();
            slot_.reset();
            if (slot) slot->active.store(false);
            if (const auto state = state_.lock())
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                const auto found = state->handlers.find(type_);
                if (found != state->handlers.end())
                {
                    auto& slots = found->second;
                    for (auto it = slots.begin(); it != slots.end(); ++it)
                        if (*it == slot) { slots.erase(it); break; }
                    if (slots.empty()) state->handlers.erase(found);
                }
            }
            state_.reset();
        }
        bool Valid() const
        {
            const auto slot = slot_.lock();
            return !state_.expired() && slot && slot->active.load();
        }
    private:
        friend class EventBus;
        Subscription(const std::shared_ptr<State>& state, const std::shared_ptr<Slot>& slot, MediaEventType type)
            : state_(state), slot_(slot), type_(type) {}
        std::weak_ptr<State> state_;
        std::weak_ptr<Slot> slot_;
        MediaEventType type_ = MediaEventType::Unknown;
    };

    EventBus() : state_(std::make_shared<State>()) {}
    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;
    ~EventBus()
    {
        decltype(State::handlers) retired;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->closed = true;
            for (const auto& handlers : state_->handlers)
                for (const auto& slot : handlers.second) slot->active.store(false);
            retired.swap(state_->handlers);
        }
        // 用户回调的捕获对象也可能持有订阅句柄，必须在锁外析构。
    }

    template <typename EventT>
    Subscription Subscribe(std::function<void(const EventT&)> callback)
    {
        if (!callback) return {};
        auto slot = std::make_shared<Slot>([callback = std::move(callback)](const MediaEvent& event) {
            if (const auto* typed = dynamic_cast<const EventT*>(&event)) callback(*typed);
        });
        const auto type = EventT::StaticType();
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->closed) return {};
        state_->handlers[type].push_back(slot);
        return Subscription(state_, slot, type);
    }

    DispatchResult Publish(const MediaEvent& event)
    {
        std::vector<std::shared_ptr<Slot>> slots;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (state_->closed) return {};
            const auto found = state_->handlers.find(event.Type());
            if (found != state_->handlers.end()) slots = found->second;
        }
        DispatchResult result;
        for (const auto& slot : slots)
        {
            if (!slot->active.load()) continue;
            try { slot->callback(event); ++result.delivered; }
            catch (...) { ++result.failed; }
        }
        return result;
    }

    // Reset 不等待已通过 active 检查的回调；捕获业务对象时应使用 weak_ptr。
    // 事件只在本次调用期间有效，异步任务必须复制数据。
    template <typename EventT, typename... Args>
    DispatchResult EmplacePublish(Args&&... args)
    {
        EventT event(std::forward<Args>(args)...);
        return Publish(event);
    }
private:
    const std::shared_ptr<State> state_;
};
}

#endif /* _EVENT_BUS_H_ */
