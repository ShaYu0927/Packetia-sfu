#pragma once
#include <cstddef>
#include <atomic>
#include <list>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace utils
{
enum class ChannelResult { Accepted, Full, Closed, Cancelled };

struct ChannelLimits
{
    std::size_t max_bytes = 4 * 1024 * 1024;
    std::size_t max_messages = 64;
};

// Deferred 在门控开启前暂存；OpenGate 先入队本条消息，再释放暂存消息。
enum class ChannelDelivery { Ready, Deferred, OpenGate };

// 多通道共用预算；计入已出队但消费回调尚未完成的消息。
class ChannelBudget
{
public:
    ChannelBudget(std::size_t bytes = 64 * 1024 * 1024, std::size_t messages = 16384)
        : max_bytes_(bytes), max_messages_(messages) {}
    bool Reserve(std::size_t bytes)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (messages_ >= max_messages_ || bytes > max_bytes_ - bytes_) return false;
        bytes_ += bytes; ++messages_; return true;
    }
    void Release(std::size_t bytes, std::size_t messages = 1)
    { std::lock_guard<std::mutex> lock(mutex_); bytes_ -= bytes; messages_ -= messages; }
    std::pair<std::size_t, std::size_t> Used() const
    { std::lock_guard<std::mutex> lock(mutex_); return {bytes_, messages_}; }
private:
    const std::size_t max_bytes_, max_messages_;
    mutable std::mutex mutex_;
    std::size_t bytes_ = 0, messages_ = 0;
};

// 多生产者、单执行器消费。每批最多 16 条，让出 owner 给网络与定时器。
// Close/Cancel 丢弃未消费消息并拒绝新发送；已开始的回调由所属 owner 串行收尾。
template<class Message>
class BoundedChannel
{
public:
    using Post = std::function<bool(std::function<void()>)>;
    using Consume = std::function<void(Message)>;
    using Complete = std::function<void()>;
    using AsyncConsume = std::function<void(Message, Complete)>;
    BoundedChannel(Post post, Consume consume, std::shared_ptr<ChannelBudget> budget,
                   ChannelLimits limits = {}, bool start_gated = false)
        : state_(std::make_shared<State>(std::move(post), std::move(consume), std::move(budget),
                                        limits.max_bytes, limits.max_messages, start_gated)) {}
    // 完成回调前保留预算，并暂停本通道后续消费，供异步消费使用。
    BoundedChannel(Post post, AsyncConsume consume, std::shared_ptr<ChannelBudget> budget,
                   ChannelLimits limits = {})
        : BoundedChannel(std::move(post), Consume{}, std::move(budget), limits)
    { state_->async_consume = std::move(consume); }
    ~BoundedChannel() { Close(); }
    BoundedChannel(const BoundedChannel&) = delete;
    BoundedChannel& operator=(const BoundedChannel&) = delete;

    ChannelResult Send(Message message, std::size_t bytes, ChannelDelivery delivery = ChannelDelivery::Ready)
    {
        auto state = state_;
        bool schedule = false;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->status != ChannelResult::Accepted) return state->status;
            if (state->messages >= state->max_messages || bytes > state->max_bytes - state->bytes ||
                !state->budget->Reserve(bytes)) return ChannelResult::Full;
            try
            {
                auto& queue = delivery == ChannelDelivery::Deferred && !state->ready ? state->deferred : state->queue;
                queue.push_back(Item{std::move(message), bytes});
            }
            catch (...) { state->budget->Release(bytes); throw; }
            state->bytes += bytes; ++state->messages;
            if (delivery == ChannelDelivery::OpenGate)
            {
                state->ready = true;
                // 开门消息已入队，再释放门控期间暂存的消息。
                while (!state->deferred.empty())
                {
                    state->queue.splice(state->queue.end(), state->deferred, state->deferred.begin());
                }
            }
            if (!state->scheduled && !state->queue.empty()) { state->scheduled = true; schedule = true; }
        }
        if (schedule && !Schedule(state)) return ChannelResult::Cancelled;
        return ChannelResult::Accepted;
    }
    void Close() { Stop(state_, ChannelResult::Closed); }
    void Cancel() { Stop(state_, ChannelResult::Cancelled); }
    ChannelResult Status() const
    { std::lock_guard<std::mutex> lock(state_->mutex); return state_->status; }
    void ResetGate()
    { std::lock_guard<std::mutex> lock(state_->mutex); state_->ready = false; }

private:
    struct Item { Message message; std::size_t bytes; };
    struct State
    {
        State(Post p, Consume c, std::shared_ptr<ChannelBudget> b, std::size_t bytes, std::size_t messages, bool gate)
            : post(std::move(p)), consume(std::move(c)), budget(std::move(b)),
              max_bytes(bytes), max_messages(messages), ready(!gate) {}
        std::mutex mutex;
        Post post;
        Consume consume;
        AsyncConsume async_consume;
        std::shared_ptr<ChannelBudget> budget;
        const std::size_t max_bytes, max_messages;
        std::size_t bytes = 0, messages = 0;
        std::list<Item> queue, deferred;
        bool scheduled = false, ready;
        ChannelResult status = ChannelResult::Accepted;
    };
    static void Stop(const std::shared_ptr<State>& state, ChannelResult status)
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->status != ChannelResult::Accepted) return;
        state->status = status;
        for (const auto& item : state->queue) { state->budget->Release(item.bytes); state->bytes -= item.bytes; --state->messages; }
        for (const auto& item : state->deferred) { state->budget->Release(item.bytes); state->bytes -= item.bytes; --state->messages; }
        state->queue.clear(); state->deferred.clear();
    }
    static bool Schedule(const std::shared_ptr<State>& state)
    {
        std::weak_ptr<State> weak = state;
        bool posted = false;
        try { posted = state->post && state->post([weak] { if (auto state = weak.lock()) Pump(state); }); }
        catch (...) {}
        if (!posted) Stop(state, ChannelResult::Cancelled);
        return posted;
    }
    static void Pump(const std::shared_ptr<State>& state)
    {
        for (int batch = 0; batch < 16; ++batch)
        {
            std::optional<Item> item;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (state->status != ChannelResult::Accepted || state->queue.empty())
                { state->scheduled = false; return; }
                item.emplace(std::move(state->queue.front()));
                state->queue.pop_front();
            }
            if (state->async_consume)
            {
                auto completed = std::make_shared<std::atomic<bool>>(false);
                const auto bytes = item->bytes;
                const auto complete = [state, completed, bytes] {
                    if (completed->exchange(true)) return;
                    bool resume;
                    {
                        std::lock_guard<std::mutex> lock(state->mutex);
                        state->bytes -= bytes; --state->messages;
                        state->budget->Release(bytes);
                        resume = state->status == ChannelResult::Accepted;
                        if (!resume) state->scheduled = false;
                    }
                    if (resume) Schedule(state);
                };
                try { state->async_consume(std::move(item->message), complete); }
                catch (...) { Stop(state, ChannelResult::Cancelled); complete(); }
                return;
            }
            bool failed = false;
            try { state->consume(std::move(item->message)); } catch (...) { failed = true; }
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->bytes -= item->bytes; --state->messages;
                state->budget->Release(item->bytes);
            }
            if (failed) { Stop(state, ChannelResult::Cancelled); return; }
        }
        Schedule(state);
    }
    std::shared_ptr<State> state_;
};
}
