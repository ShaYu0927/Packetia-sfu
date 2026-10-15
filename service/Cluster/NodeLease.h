#pragma once
#include "service/Auth/AuthTypes.h"
#include <atomic>
#include <chrono>
#include <string>

namespace service::cluster
{
// Immutable target, monotonic local lease; safe for heartbeat and I/O threads.
class NodeLease final
{
public:
    NodeLease(std::string node, std::string instance) : node_(std::move(node)), instance_(std::move(instance)) {}
    static int64_t NowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    void Renew(int64_t request_started_ms, int lease_seconds)
    {
        // Count from request START, leaving two seconds for transport/clock rounding.
        deadline_.store(request_started_ms + (lease_seconds - 2) * 1000);
    }
    void Revoke() { deadline_.store(0); }
    bool Allows(const auth::AuthContext& context) const
    {
        return context.node_id == node_ && context.instance_id == instance_ && NowMs() < deadline_.load();
    }
private:
    const std::string node_, instance_;
    std::atomic<int64_t> deadline_{0};
};
}
