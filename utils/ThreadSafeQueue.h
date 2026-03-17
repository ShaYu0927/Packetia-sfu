//
// Created by roots on 2024/9/12.
//

#ifndef FFMPEGAAC_THREADSAFEQUEUE_H
#define FFMPEGAAC_THREADSAFEQUEUE_H

#include <functional>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <thread>

template<typename T>
class ThreadSafeQueue
{
public:
    ThreadSafeQueue() = default;

    ThreadSafeQueue(const ThreadSafeQueue& other)
    {
        std::lock_guard<std::mutex> lock(other.m_mutex);
        m_queue = other.m_queue;
    }

    ThreadSafeQueue& operator=(const ThreadSafeQueue& other)
    {
        if (this == &other)
            return *this;

        std::scoped_lock lock(m_mutex, other.m_mutex);
        m_queue = other.m_queue;
        return *this;
    }

    void push(T value)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_queue.push(std::move(value));
        }
        m_cond.notify_one();
    }

    void waitAndPop(T& value)
    {
        std::unique_lock<std::mutex> lock(m_mutex);

        m_cond.wait(lock, [this] {
            return !m_queue.empty();
        });

        value = std::move(m_queue.front());
        m_queue.pop();
    }

    std::shared_ptr<T> waitAndPop()
    {
        std::unique_lock<std::mutex> lock(m_mutex);

        m_cond.wait(lock, [this] {
            return !m_queue.empty();
        });

        auto value = std::make_shared<T>(std::move(m_queue.front()));
        m_queue.pop();
        return value;
    }

    bool tryPop(T& value)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_queue.empty())
            return false;

        value = std::move(m_queue.front());
        m_queue.pop();
        return true;
    }

    std::shared_ptr<T> tryPop()
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_queue.empty())
            return nullptr;

        auto value = std::make_shared<T>(std::move(m_queue.front()));
        m_queue.pop();
        return value;
    }

    bool empty() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_queue.empty();
    }

    void clear()
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        std::queue<T> empty;
        std::swap(m_queue, empty);
    }

    size_t size() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_queue.size();
    }

private:
    mutable std::mutex m_mutex;
    std::queue<T> m_queue;
    std::condition_variable m_cond;
};

class QueueManager
{
public:
    using Task = std::function<void()>;

    static QueueManager& Instance()
    {
        static QueueManager inst;
        return inst;
    }

    void Start(size_t worker_num);
    void Post(size_t key, Task task);

private:
    std::vector<ThreadSafeQueue<Task>> queues;
    std::vector<std::thread> workers;

    void WorkerLoop(size_t idx)
    {
        auto& q = queues[idx];

        while (true)
        {
            Task task;
            q.waitAndPop(task);
            task();
        }
    }
};


#endif //FFMPEGAAC_THREADSAFEQUEUE_H
