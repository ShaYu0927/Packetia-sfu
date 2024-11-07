//
// Created by roots on 2024/9/12.
//

#ifndef FFMPEGAAC_THREADSAFEQUEUE_H
#define FFMPEGAAC_THREADSAFEQUEUE_H

#include <queue>
#include <memory>

template<typename T>
class ThreadSafeQueue {
    ThreadSafeQueue()
    {

    }

    ThreadSafeQueue(const ThreadSafeQueue& other)
    {
        std::lock_guard<std::mutex> lock(other.m_mutex);
        m_queue = other.m_queue;
    }

    ThreadSafeQueue& operator=(const ThreadSafeQueue& other)
    {
        m_queue = other.m_queue;
    }

    ~ThreadSafeQueue()
    {

    }

    void push(T& data)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push(data);
        m_condition.notify_one();
    }

    bool waiAndPop(T& data)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_condition.wait(lock);
        value = m_queue.front();
        m_queue.pop();
        return true;
    }

    bool tryPop(T& data)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if(m_queue.empty())
        {
            return false;
        }
        data = m_queue.front();
        m_queue.pop();
        return true;
    }

    std::shared_ptr<T> tryPop()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if(m_queue.empty())
        {
            return std::shared_ptr<T>();
        }
        std::shared_ptr<T> value(std::make_shared<T>(m_queue.front()));
        m_queue.pop();
        return value;
    }

    bool empty()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_queue.empty();
    }

    void clear()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        while(!m_queue.empty())
        {
            m_queue.pop();
        }
    }

    int size()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_queue.size();
    }
private:

    std::queue<T> m_queue;
    std::mutex m_mutex;
    std:condition_variable m_condition;
};

#endif //FFMPEGAAC_THREADSAFEQUEUE_H
