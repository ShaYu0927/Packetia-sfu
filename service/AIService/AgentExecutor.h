#pragma once

#include "ThreadManager.h"
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>

namespace service::ai
{
// 一个 Agent 服务共用一个专属线程。只执行短控制任务，不在这里等待模型或网络。
class AgentExecutor final
{
public:
    explicit AgentExecutor(std::size_t max_pending = 128) : max_pending_(max_pending) {}
    ~AgentExecutor();
    bool Start(std::function<void()> on_stop);
    // 禁止新任务，取消尚未执行的任务，等待当前任务及 on_stop 完成。
    // 生命周期由外部管理线程调用，不能在 Agent 线程内 Stop。
    void Stop();
    void PostCompletion(std::function<void()> completion) const;

    template<class Function>
    auto Submit(Function function) const -> std::future<std::invoke_result_t<Function>>
    {
        using Result = std::invoke_result_t<Function>;
        auto task = std::make_shared<std::packaged_task<Result(bool)>>(
            [function = std::move(function)](bool execute) mutable -> Result {
                if (!execute) RejectUnavailable();
                return function();
            });
        auto result = task->get_future();
        Enqueue([task](bool execute) { (*task)(execute); });
        return result;
    }

private:
    using Command = std::function<void(bool)>;
    [[noreturn]] static void RejectUnavailable();
    void Enqueue(Command command) const;
    void RequestStop();
    void Run();

    const std::size_t max_pending_;
    std::mutex lifecycle_mutex_;
    mutable std::mutex mutex_;
    mutable std::condition_variable wakeup_;
    mutable std::deque<Command> pending_;
    bool accepting_ = false;
    std::function<void()> on_stop_;
    utils::ManagedThread thread_;
    inline static thread_local const AgentExecutor* current_ = nullptr;
};
}
