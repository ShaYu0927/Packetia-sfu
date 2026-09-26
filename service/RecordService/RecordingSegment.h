#ifndef PACKETIA_SERVICE_RECORDSERVICE_RECORDINGSEGMENT_H_
#define PACKETIA_SERVICE_RECORDSERVICE_RECORDINGSEGMENT_H_

#include "Mp4Writer.h"
#include "RecordingTypes.h"
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace service 
{
/**
 * @brief 表示一个 MP4 录像分段
 *
 * 保存当前录像分段的写入器、轨道时钟、待写入帧、
 * 时间信息、运行状态以及事件上报状态。
 *
 * 该类由 Mp4Recorder 管理，不允许被继承。
 */
class RecordingSegment final
{
public:
    /**
     * @brief 判断录像写入器是否已经创建
     *
     * 注意：这里只判断 writer_ 是否为空，
     * 不代表底层文件一定处于正常可写状态。
     */
    bool IsOpen() const noexcept { return writer_ != nullptr; }

     /**
     * @brief 判断当前录像分段是否发生过失败
     */
    bool HasFailed() const noexcept { return failed_; }

     /**
     * @brief 获取当前录像分段的状态
     */
    RecordingSegmentState State() const noexcept { return state_; }

private:
    friend class Mp4Recorder;

     /**
     * @brief 单条音频或视频轨道的时钟信息
     *
     * 用于根据 RTP 时间戳生成连续的 MP4 时间轴，
     * 并处理不同轨道之间的时间对齐。
     */
    struct Clock
    {
        media::EncodedFrameEvent first;   /* 当前轨道收到的第一帧事件 */
        uint32_t previous = 0;            /* 上一帧的 RTP 时间戳    */
        int64_t ticks = 0;       
        int64_t anchor_us = 0;            /* 当前轨道在统一录像时间轴上的锚点，单位为微秒 */
    };

    std::map<RecordingTrackKey, Clock> tracks;
    std::vector<media::EncodedFrameEvent> pending;   /* 暂时不能写入 MP4 的编码帧 */
    std::unique_ptr<Mp4Writer> writer_;              /* MP4 文件写入器，由当前录像分段独占 */
    std::string path;
    std::string tmp_path;                               
    uint64_t first_ms = 0;                           /* 当前分段第一帧的时间，单位为毫秒。 */
    uint64_t last_ms = 0;                            /* 当前分段最后一帧的时间，单位为毫秒 */
    uint64_t frames = 0;                             /* 当前分段已经处理或写入的帧数      */
    int64_t origin_us = 0;                           /* 当前录像分段统一时间轴的起点，单位为微秒 */
    size_t pending_bytes = 0;                        /* pending 缓存中所有帧占用的总字节数 */
    bool video_seen = false;
    bool failed_ = false;
    RecordingSegmentState state_ = RecordingSegmentState::Created;    /* 当前录像分段的生命周期状态，初始为 Created */
    std::string session_id;                                           /* 当前录像所属的会话 ID。 */
    std::string stream_id;
    bool start_event_sent = false;                                    /* 是否已经发送过“录像开始”事件，防止重复上报 */
    bool terminal_event_sent = false;                                 /* 是否已经发送过最终事件，例如完成或失败，防止重复上报 */
};

}

#endif // PACKETIA_SERVICE_RECORDSERVICE_RECORDINGSEGMENT_H_
