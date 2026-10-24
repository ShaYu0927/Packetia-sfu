#pragma once
#include "MetricTypes.h"
#include <memory>
#include <mutex>

namespace monitoring
{
class WorkerMetrics
{
public:
    WorkerMetrics(std::string pool, size_t index, std::vector<std::string> task_names = {});
    void Queue(size_t depth, size_t bytes, uint64_t oldest_ns, uint64_t dropped = 0);
    void Begin(uint64_t enqueued_ns, size_t type, uint64_t now);
    void End(size_t type, uint64_t now);
    WorkerSnapshot Snapshot(uint64_t now) const;
private:
    mutable std::mutex mutex_;
    WorkerSnapshot state_;
    BusyWindow busy_;
};
class SendQueueMetrics
{
public:
    SendQueueMetrics(uint64_t id, std::string transport);
    void Set(uint64_t bytes, uint64_t packets, uint64_t oldest_ns);
    SendQueueSnapshot Snapshot() const;
private:
    mutable std::mutex mutex_;
    SendQueueSnapshot state_;
};
class MetricRegistry
{
public:
    MetricRegistry();
    ~MetricRegistry();
    MetricRegistry(const MetricRegistry&) = delete;
    MetricRegistry& operator=(const MetricRegistry&) = delete;
    std::shared_ptr<WorkerMetrics> RegisterWorker(const std::string& pool, size_t index,
                                                std::vector<std::string> task_names = {});
    std::shared_ptr<SendQueueMetrics> RegisterSendQueue(const std::string& transport);
    void RecordSend(Transport transport, SendEvent event, uint64_t bytes = 0, uint64_t packets = 0);
    RuntimeSnapshot Collect() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// The application owns the registry. This scoped process binding adapts existing
// globally managed workers/sockets to it; it contains no process-lifetime metrics.
// One active application context per process, matching WorkerService ownership.
class MetricsContext
{
public:
    explicit MetricsContext(bool activate = true);
    ~MetricsContext();
    MetricsContext(const MetricsContext&) = delete;
    MetricsContext& operator=(const MetricsContext&) = delete;
    std::shared_ptr<MetricRegistry> Registry() const { return registry_; }
    bool Activate();
    void Deactivate();
private:
    std::shared_ptr<MetricRegistry> registry_;
};
std::shared_ptr<WorkerMetrics> RegisterWorker(const std::string& pool, size_t index,
                                            std::vector<std::string> task_names = {});
std::shared_ptr<SendQueueMetrics> RegisterSendQueue(const std::string& transport);
void RecordSend(Transport transport, SendEvent event, uint64_t bytes = 0, uint64_t packets = 0);
class TaskScope
{
public:
    TaskScope(WorkerMetrics* worker, uint64_t enqueued_ns, size_t type);
    ~TaskScope();
    TaskScope(const TaskScope&) = delete;
    TaskScope& operator=(const TaskScope&) = delete;
private:
    WorkerMetrics* worker_;
    size_t type_;
};
}
