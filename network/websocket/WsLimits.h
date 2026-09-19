#pragma once

#include <cstddef>
#include <mutex>

namespace network
{
struct WsLimits
{
    std::size_t max_connections = 4096;
    std::size_t max_message_bytes = 1024 * 1024;
    std::size_t max_queued_bytes = 4 * 1024 * 1024;
    std::size_t max_queued_messages = 256;
    std::size_t max_total_queued_bytes = 64 * 1024 * 1024;
    std::size_t max_total_queued_messages = 16384;
};

struct WsSendStats
{
    std::size_t bytes = 0;
    std::size_t messages = 0;
};

class WsSendAccount;

// Shared by every session of a server. Reservations include messages that
// have left the application queue but have not finished being written.
class WsSendBudget
{
public:
    WsSendBudget(std::size_t max_bytes, std::size_t max_messages)
        : max_bytes_(max_bytes), max_messages_(max_messages) {}

    WsSendStats GetStats() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return used_;
    }

private:
    friend class WsSendAccount;

    bool Reserve(std::size_t bytes)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (used_.messages >= max_messages_ || bytes > max_bytes_ - used_.bytes)
            return false;
        used_.bytes += bytes;
        ++used_.messages;
        return true;
    }

    void Release(std::size_t bytes, std::size_t messages)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        used_.bytes -= bytes;
        used_.messages -= messages;
    }

    const std::size_t max_bytes_, max_messages_;
    mutable std::mutex mutex_;
    WsSendStats used_;
};
}
