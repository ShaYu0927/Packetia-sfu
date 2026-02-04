#ifndef _CLIENT_SESSION_H_
#define _CLIENT_SESSION_H_

#include <condition_variable>
#include <memory>

#include "RtpConnection.h"
#include "Rtp.h"

class ISession 
{
public:
    virtual ~ISession() = default;
    virtual void Start() = 0;
    virtual void Stop() = 0;
};

class ClientSession : public ISession
{
public:
    using Ptr = std::shared_ptr<ClientSession>;

    explicit ClientSession(std::shared_ptr<RtpConnection> conn)
        : connection_(std::move(conn)) {}

    ~ClientSession() override;

    void Start() override;

    void Stop() override;

    void Enqueue(const RtpPacket& pkt);

    uint64_t DropCount() const { return drop_.load(); }

private:
    std::shared_ptr<RtpConnection> connection_;

    std::deque<RtpPacket> queue_;
    mutable std::mutex mtx_;
    std::condition_variable cv_;

    std::thread send_thread_;
    std::atomic_bool running_{false};

    size_t max_queue_{2000};            // 必须限长
    std::atomic<uint64_t> drop_{0};     // 丢包计数
};


#endif