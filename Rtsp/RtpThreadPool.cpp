#include "RtpThreadPool.h"

RtpThreadPool::RtpThreadPool(size_t numThreads = 4)
{
    for(size_t i = 0; i < numThreads; ++i)
    {
        rtp_threads_.emplace_back([i, this]() {
            LOG_INFO("RTP Thread " + std::to_string(i) + " started.");

            while (true)
            {
                std::function<void()> task;

                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });

                    if (stop_ && tasks_.empty()) {
                        LOG_INFO("RTP Thread " + std::to_string(i) + " stopping.");
                        return;
                    }

                    task = std::move(tasks_.front());
                    tasks_.pop();
                } // lock 释放

                task();  
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
    }

}

RtpThreadPool::~RtpThreadPool()
{
}
