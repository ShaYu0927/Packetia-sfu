#ifndef _ENDPOINT_H_
#define _ENDPOINT_H_


#include <atomic>
#include <cstdint>
#include <string>

#include "ShardedWorkerPool.h"
namespace utils 
{


class EndpointBase
{
public:
    enum class State : std::uint8_t
    {
        kInit = 0,
        kRunning,
        kStopping,
        kStopped
    };

public:
    EndpointBase(std::uint64_t endpoint_id, std::string name)
        : endpoint_id_(endpoint_id), name_(std::move(name))
    {
    }

    virtual ~EndpointBase() = default;

    std::uint64_t Id() const
    {
        return endpoint_id_;
    }

    const std::string& Name() const
    {
        return name_;
    }

    State GetState() const
    {
        return state_.load(std::memory_order_relaxed);
    }

    bool IsRunning() const
    {
        return state_.load(std::memory_order_relaxed) == State::kRunning;
    }

    static std::uint64_t NextEndpointId()
    {
        static std::atomic<std::uint64_t> s_id {1};
        return s_id.fetch_add(1, std::memory_order_relaxed);
    }

    virtual bool Start() = 0;
    virtual void Stop() = 0;
    virtual void ProcessJob(WorkJob& job);

protected:
    virtual void OnRtp(WorkJob& job)  {}
    virtual void OnRtcp(WorkJob& job) {}
    virtual void OnStun(WorkJob& job) {}
    virtual void OnDtls(WorkJob& job) {}
    virtual void OnRtsp(WorkJob& job) {}
    virtual void OnFunction(WorkJob& job) {}
    virtual void OnUnknown(WorkJob& job) {}
   
    void SetState(State s)
    {
        state_.store(s, std::memory_order_relaxed);
    }

  

protected:
    std::uint64_t endpoint_id_{0};
    std::string name_;
    std::atomic<State> state_{State::kInit};
};

class EndpointManager
{
public:
    static EndpointManager& Instance()
    {
        static EndpointManager instance;
        return instance;
    }

    std::uint64_t AllocId();

    bool Add(const std::shared_ptr<EndpointBase>& endpoint);

    std::shared_ptr<EndpointBase> Find(std::uint64_t endpoint_id);

    bool Remove(std::uint64_t endpoint_id);
    bool Exists(std::uint64_t endpoint_id);

    std::size_t Size() const;

    void Clear();

private:
    EndpointManager() = default;
    ~EndpointManager() = default;

    EndpointManager(const EndpointManager&) = delete;
    EndpointManager& operator=(const EndpointManager&) = delete;
    EndpointManager(EndpointManager&&) = delete;
    EndpointManager& operator=(EndpointManager&&) = delete;

private:
    mutable std::mutex mtx_;
    std::unordered_map<std::uint64_t, std::shared_ptr<EndpointBase>> endpoints_;
    std::atomic<std::uint64_t> next_id_{1};
};

class EndpointJobHandler : public IJobHandler
{
public:
    explicit EndpointJobHandler(EndpointManager* mgr)
        : mgr_(mgr)
    {
    }

    ~EndpointJobHandler() override = default;
    void handle(WorkJob& job) override;


private:
    EndpointManager* mgr_{nullptr};
};

}
#endif /* _ENDPOINT_H_ */
