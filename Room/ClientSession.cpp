#include "ClientSession.h"


void RClientSession::Start()
{
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) 
    {
        return; // already running
    }

    send_thread_ = std::thread([this] {
        for (;;) {
            RtpPacket pkt;

            {
                std::unique_lock<std::mutex> lk(mtx_);
                cv_.wait(lk, [&] { return !running_.load() || !queue_.empty(); });

                if (!running_.load() && queue_.empty())
                    break;

                pkt = std::move(queue_.front());
                queue_.pop_front();
            }
            // connection_->SendRtp(pkt);
        }
    });
}

void RClientSession::Stop()
{
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) 
    {
        return; 
    }

    cv_.notify_all();

    if (send_thread_.joinable())
        send_thread_.join();

    {
        std::lock_guard<std::mutex> lk(mtx_);
        queue_.clear();
    }
}

void RClientSession::Enqueue(const RtpPacket& pkt)
{
    std::lock_guard<std::mutex> lk(mtx_);

    if (!running_.load()) 
    {
        drop_++;
        return;
    }

    if (queue_.size() >= max_queue_) 
    {
        drop_++;
        return;
    }

    queue_.push_back(pkt);
    cv_.notify_one();
}

