#pragma once
#include <cstddef>
#include <list>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace server::signaling
{
enum class SignalResult { Accepted, Full, Closed, Cancelled };

// 两个方向共用全局预算；计入已出队但消费回调尚未完成的消息。
class SignalBudget
{
public:
    SignalBudget(std::size_t bytes = 64 * 1024 * 1024, std::size_t messages = 16384)
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
class SignalChannel
{
public:
    using Post = std::function<bool(std::function<void()>)>;
    using Consume = std::function<void(Message)>;
    SignalChannel(Post post, Consume consume, std::shared_ptr<SignalBudget> budget,
                  std::size_t max_bytes = 4 * 1024 * 1024, std::size_t max_messages = 64,
                  bool gate_notifications = false)
        : state_(std::make_shared<State>(std::move(post), std::move(consume), std::move(budget),
                                        max_bytes, max_messages, gate_notifications)) {}
    ~SignalChannel() { Close(); }
    SignalChannel(const SignalChannel&) = delete;
    SignalChannel& operator=(const SignalChannel&) = delete;

    SignalResult Send(Message message, std::size_t bytes, bool notification = false, bool initial = false)
    {
        auto state = state_;
        bool schedule = false;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->status != SignalResult::Accepted) return state->status;
            if (state->messages >= state->max_messages || bytes > state->max_bytes - state->bytes ||
                !state->budget->Reserve(bytes)) return SignalResult::Full;
            try
            {
                auto& queue = notification && !state->ready ? state->deferred : state->queue;
                queue.push_back(Item{std::move(message), bytes});
            }
            catch (...) { state->budget->Release(bytes); throw; }
            state->bytes += bytes; ++state->messages;
            if (initial)
            {
                state->ready = true;
                // 初始响应已入队，再释放就绪前的主动通知。
                while (!state->deferred.empty())
                {
                    state->queue.splice(state->queue.end(), state->deferred, state->deferred.begin());
                }
            }
            if (!state->scheduled && !state->queue.empty()) { state->scheduled = true; schedule = true; }
        }
        if (schedule && !Schedule(state)) return SignalResult::Cancelled;
        return SignalResult::Accepted;
    }
    void Close() { Stop(state_, SignalResult::Closed); }
    void Cancel() { Stop(state_, SignalResult::Cancelled); }
    SignalResult Status() const
    { std::lock_guard<std::mutex> lock(state_->mutex); return state_->status; }
    void ResetNotificationGate()
    { std::lock_guard<std::mutex> lock(state_->mutex); state_->ready = false; }

private:
    struct Item { Message message; std::size_t bytes; };
    struct State
    {
        State(Post p, Consume c, std::shared_ptr<SignalBudget> b, std::size_t bytes, std::size_t messages, bool gate)
            : post(std::move(p)), consume(std::move(c)), budget(std::move(b)),
              max_bytes(bytes), max_messages(messages), ready(!gate) {}
        std::mutex mutex;
        Post post;
        Consume consume;
        std::shared_ptr<SignalBudget> budget;
        const std::size_t max_bytes, max_messages;
        std::size_t bytes = 0, messages = 0;
        std::list<Item> queue, deferred;
        bool scheduled = false, ready;
        SignalResult status = SignalResult::Accepted;
    };
    static void Stop(const std::shared_ptr<State>& state, SignalResult status)
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->status != SignalResult::Accepted) return;
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
        if (!posted) Stop(state, SignalResult::Cancelled);
        return posted;
    }
    static void Pump(const std::shared_ptr<State>& state)
    {
        for (int batch = 0; batch < 16; ++batch)
        {
            std::optional<Item> item;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (state->status != SignalResult::Accepted || state->queue.empty())
                { state->scheduled = false; return; }
                item.emplace(std::move(state->queue.front()));
                state->queue.pop_front();
            }
            bool failed = false;
            try { state->consume(std::move(item->message)); } catch (...) { failed = true; }
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->bytes -= item->bytes; --state->messages;
                state->budget->Release(item->bytes);
            }
            if (failed) { Stop(state, SignalResult::Cancelled); return; }
        }
        Schedule(state);
    }
    std::shared_ptr<State> state_;
};
}
