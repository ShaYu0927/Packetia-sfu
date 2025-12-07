#pragma once

#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <functional>
#include <memory>
#include <iostream>
#include "logger.h"

class RtpThreadPool 
{
public:
    static void init();
    static void stopAll();

    RtpThreadPool(size_t numThreads);
    ~RtpThreadPool();


    void post(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tasks_.push(std::move(task));
        }
        cv_.notify_one();
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto &t : rtp_threads_) if (t.joinable()) t.join();
    }

private:
    std::vector<std::thread> rtp_threads_;
    std::mutex mutex_;
    std::condition_variable cv_;  
    bool stop_ = false;
    std::queue<std::function<void()>> tasks_;
};