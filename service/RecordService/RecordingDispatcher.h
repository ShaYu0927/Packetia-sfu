#pragma once

#include "RecordingSession.h"
#include "ShardedWorkerPool.h"
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace service 
{

// Recording business module running on the project's ShardedWorkerPool.
// Admission is concurrent; each recorder is owned exclusively by one shard.
class RecordingDispatcher final
{
public:
    /**
     * @brief 当前未完成录制任务的统计信息。
     */
    struct QueueStats { size_t frames = 0, bytes = 0; };


     /**
     * @brief 构造录制任务分发器。
     *
     * @param options  录制配置，例如队列限制、输出路径和录制参数等。
     * @param context  录制业务上下文，提供线程池及相关共享资源。
     * @param accepted 成功接收并提交到工作线程池的帧计数器。
     * @param dropped  因停止接收、队列超限等原因丢弃的帧计数器。
     * @param errors   录制任务处理失败的计数器。
     *
     * @note 外部传入的原子计数器必须比 RecordingDispatcher 生命周期更长。
     */
    RecordingDispatcher(const RecordingOptions& options,
        std::shared_ptr<RecordingContext> context,
        std::atomic<uint64_t>& accepted,
        std::atomic<uint64_t>& dropped,
        std::atomic<uint64_t>& errors);
    ~RecordingDispatcher();
    RecordingDispatcher(const RecordingDispatcher&) = delete;
    RecordingDispatcher& operator=(const RecordingDispatcher&) = delete;
    
     /**
     * @brief 启动录制任务分发器。
     *
     * 初始化任务处理器，并允许 Post() 接收新的音视频帧。
     */
    void Start();

    /**
     * @brief 停止录制任务分发器。
     *
     * 停止接收新的音视频帧，并处理录制流退出及相关资源清理。
     */
    void Stop();

     /**
     * @brief 提交一帧编码后的音视频数据。
     *
     * 根据事件中的流标识找到对应的 StreamEntry，并将帧任务投递到
     * 固定的 worker shard，保证同一路流中的帧按顺序处理。
     *
     * @param event 编码后的音频或视频帧事件。
     *
     * @return true  帧已成功接收并提交。
     * @return false 当前未接收任务、队列超限或任务提交失败。
     */
    bool Post(const media::EncodedFrameEvent& event);

    /**
     * @brief 获取当前任务队列统计信息。
     *
     * @return 当前未完成的帧数量和总字节数。
     */
    QueueStats Stats() const;

private:
    /**
     * @brief 录制流的唯一标识。
     *
     * 两个字符串的具体含义由业务决定，例如：
     * {session_id, stream_id}、{room_id, track_id} 等。
     */
    using Key = std::pair<std::string, std::string>;

     /**
     * @brief 一路录制流对应的运行状态。
     *
     * 通常保存 Recorder 实例、待处理任务数以及退出状态等信息。
     * 具体成员在实现文件中定义。
     */
    struct StreamEntry;
     /**
     * @brief 单帧录制任务。
     *
     * 封装待处理的编码帧及其所属录制流信息。
     */
    struct FrameJob;

     /**
     * @brief 工作线程池任务处理器。
     *
     * 在对应的 worker shard 中处理 FrameJob。
     */
    class JobHandler;
    
    /**
     * @brief 计算录制流对应的线程亲和键。
     *
     * 相同 Key 会生成相同的 affinity key，从而被分配到同一个 shard。
     *
     * @param key 录制流唯一标识。
     * @return 用于 ShardedWorkerPool 分片选择的哈希值。
     */
    static uint64_t AffinityKey(const Key& key);

    /**
     * @brief 完成一帧任务后的收尾处理。
     *
     * 用于更新待处理帧数、字节数以及 StreamEntry 中的任务状态。
     *
     * @param entry 该帧所属的录制流。
     * @param bytes 本次处理完成的帧字节数。
     */
    void Complete(const std::shared_ptr<StreamEntry>& entry, size_t bytes);

    /**
     * @brief 退出并清理指定的录制流。
     *
     * 当录制流结束且相关任务处理完成后，将其从 streams_ 中移除，
     * 并释放对应的 Recorder 资源。
     *
     * @param key   录制流唯一标识。
     * @param entry 对应的录制流状态对象。
     */
    void Retire(const Key& key, const std::shared_ptr<StreamEntry>& entry);

    const RecordingOptions options_;
    std::shared_ptr<RecordingContext> context_;
    std::atomic<uint64_t>& accepted_;
    std::atomic<uint64_t>& dropped_;
    std::atomic<uint64_t>& errors_;
    mutable std::mutex mutex_;
    bool accepting_ = false;
    QueueStats queued_;
    std::map<Key, std::shared_ptr<StreamEntry>> streams_;
    std::shared_ptr<JobHandler> handler_;
    bool started_ = false;
};

}
