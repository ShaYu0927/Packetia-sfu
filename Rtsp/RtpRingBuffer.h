// rtp_input_workers.h
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <vector>

// --- 简单 Packet 和 PacketPool ---
struct Packet {
    uint8_t* data = nullptr;        // 指向 payload 起始（不含 $... 4 字节）
    uint16_t length = 0;
    uint8_t channel = 0;
    // 当 packet->own_memory == true 时，data 指向 pool 分配的内存，需要释放回 pool
    bool own_memory = false;
    std::shared_ptr<void> owner_ref; // 用于持有外部 buffer（zero-copy 情况），防止早释放
};

class PacketPool {
public:
    PacketPool(size_t buf_size, size_t pool_size) : _buf_size(buf_size) {
        for (size_t i = 0; i < pool_size; ++i) {
            auto mem = new uint8_t[buf_size];
            _free_buffers.push(mem);
        }
    }
    ~PacketPool() {
        while (!_free_buffers.empty()) {
            delete[] _free_buffers.front();
            _free_buffers.pop();
        }
    }

    // 获取一个内存块（可能返回 nullptr）
    uint8_t* acquire() {
        std::lock_guard<std::mutex> lk(_mtx);
        if (_free_buffers.empty()) return nullptr;
        uint8_t* p = _free_buffers.front();
        _free_buffers.pop();
        return p;
    }
    void release(uint8_t* p) {
        if (!p) return;
        std::lock_guard<std::mutex> lk(_mtx);
        _free_buffers.push(p);
    }
    size_t bufferSize() const { return _buf_size; }

private:
    size_t _buf_size;
    std::mutex _mtx;
    std::queue<uint8_t*> _free_buffers;
};

// --- 简单线程安全队列（worker queue） ---
template<typename T>
class TSQueue {
public:
    void push(T v) {
        {
            std::lock_guard<std::mutex> lk(_mtx);
            _q.push(std::move(v));
        }
        _cv.notify_one();
    }
    bool pop(T &out) {
        std::unique_lock<std::mutex> lk(_mtx);
        _cv.wait(lk, [&](){ return !_q.empty() || _stop; });
        if (_q.empty()) return false;
        out = std::move(_q.front()); _q.pop();
        return true;
    }
    void notify_stop() {
        {
            std::lock_guard<std::mutex> lk(_mtx);
            _stop = true;
        }
        _cv.notify_all();
    }
private:
    std::queue<T> _q;
    std::mutex _mtx;
    std::condition_variable _cv;
    bool _stop = false;
};

// --- Worker: 负责处理 packet（拼帧/分发到 tracker） ---
class ITracker {
public:
    virtual ~ITracker() = default;
    // 必须实现：如果需要长期保存 data，应复制；若仅临时解析，可直接访问 data 指针
    virtual void inputRtp(uint8_t mediaType, uint32_t sampleRate, const uint8_t* data, uint16_t len) = 0;
};

class Worker {
public:
    Worker(PacketPool &pool) : _pool(pool), _running(true) {
        _thread = std::thread([this]{ this->run(); });
    }
    ~Worker() {
        _running = false;
        _queue.notify_stop();
        if (_thread.joinable()) _thread.join();
    }

    void post(Packet pkt) {
        _queue.push(std::move(pkt));
    }

    // 注册或查询 channel -> tracker (你可改成从 MediaSessionManager 查询)
    void registerTracker(uint8_t channel, std::shared_ptr<ITracker> tracker) {
        std::lock_guard<std::mutex> lk(_track_mtx);
        _trackers[channel] = tracker;
    }

private:
    void run() {
        while (_running) {
            Packet pkt;
            if (!_queue.pop(pkt)) break;
            // 找 tracker
            std::shared_ptr<ITracker> tr;
            {
                std::lock_guard<std::mutex> lk(_track_mtx);
                auto it = _trackers.find(pkt.channel);
                if (it != _trackers.end()) tr = it->second;
            }
            if (!tr) {
                // 没找到 tracker，丢弃
                if (pkt.own_memory) _pool.release(pkt.data);
                continue;
            }

            // 直接调用 tracker
            tr->inputRtp(/*mediaType*/0, /*sampleRate*/90000, pkt.data, pkt.length);

            // 释放内存（如果我们 own）
            if (pkt.own_memory) _pool.release(pkt.data);
        }
    }

    PacketPool &_pool;
    TSQueue<Packet> _queue;
    std::atomic<bool> _running;
    std::thread _thread;
    std::mutex _track_mtx;
    std::unordered_map<uint8_t, std::shared_ptr<ITracker>> _trackers;
};

// --- WorkerPool: hash 分流，保证同一 channel 落到同一 worker ---
class WorkerPool {
public:
    WorkerPool(size_t worker_count, PacketPool &pool) {
        for (size_t i=0;i<worker_count;++i) {
            _workers.emplace_back(std::make_unique<Worker>(pool));
        }
    }
    void postPacket(uint8_t channel, Packet pkt) {
        size_t idx = channel % _workers.size();
        _workers[idx]->post(std::move(pkt));
    }
    // 如果你想把 tracker 注册到特定 worker（保持 channel 对应），可以：
    void registerTracker(uint8_t channel, std::shared_ptr<ITracker> tr) {
        size_t idx = channel % _workers.size();
        _workers[idx]->registerTracker(channel, tr);
    }

private:
    std::vector<std::unique_ptr<Worker>> _workers;
};




