#include "AgentExecutor.h"
#include "ClusterTypes.h"
#include <stdexcept>

namespace service::ai
{
AgentExecutor::~AgentExecutor() { Stop(); }

bool AgentExecutor::Start(std::function<void()> on_stop)
{
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
    if (!max_pending_ || thread_.Joinable()) return false;
    on_stop_ = std::move(on_stop);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        accepting_ = true;
    }
    try
    {
        thread_ = utils::ThreadManager::Instance().Start(
            {"agent", "voice-agent", utils::ThreadModel::Dedicated},
            [this] { Run(); }, [this] { RequestStop(); });
    }
    catch (...)
    {
        RequestStop();
        // Start 失败也必须完成并发 Submit 的 future。
        std::deque<Command> cancelled;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cancelled.swap(pending_);
        }
        for (auto& command : cancelled) command(false);
        on_stop_ = {};
        throw;
    }
    return true;
}

void AgentExecutor::Stop()
{
    if (current_ == this) throw std::logic_error("Agent executor cannot join itself");
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
    RequestStop();
    thread_.Join();
    on_stop_ = {};
}

void AgentExecutor::RejectUnavailable() { throw cluster::Failure(503, "agent_unavailable"); }

void AgentExecutor::Enqueue(Command command) const
{
    // 同步业务接口会等待 future；重入时直接拒绝，防止等待自己。
    if (current_ == this) throw std::logic_error("Agent executor does not accept reentrant commands");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!accepting_) RejectUnavailable();
        if (pending_.size() >= max_pending_) throw cluster::Failure(503, "agent_busy");
        pending_.push_back(std::move(command));
    }
    wakeup_.notify_one();
}

void AgentExecutor::PostCompletion(std::function<void()> completion) const
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!accepting_) return;
        pending_.push_back([completion = std::move(completion)](bool execute) {
            if (execute) completion();
        });
    }
    wakeup_.notify_one();
}

void AgentExecutor::RequestStop()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        accepting_ = false;
    }
    wakeup_.notify_one();
}

void AgentExecutor::Run()
{
    current_ = this;
    for (;;)
    {
        Command command;
        bool execute = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wakeup_.wait(lock, [this] { return !accepting_ || !pending_.empty(); });
            if (pending_.empty()) break;
            execute = accepting_;
            command = std::move(pending_.front());
            pending_.pop_front();
        }
        // packaged_task 将业务异常传回调用者，不终止 Agent 线程。
        command(execute);
    }
    if (on_stop_) on_stop_();
    current_ = nullptr;
}
}
