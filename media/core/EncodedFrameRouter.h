#ifndef _ENCODED_FRAME_ROUTER_H_
#define _ENCODED_FRAME_ROUTER_H_

#include "../MediaFrame/MediaFrame.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace media
{

/**
 * @brief 编码帧的来源信息。
 *
 * 用于标识一帧编码后的音视频数据来自哪个端点、会话、
 * 媒体流以及具体的媒体轨道。
 */

struct EncodedFrameSource
{

    /**
     * @brief 媒体端点的唯一标识。
     *
     * 可以表示一个客户端、参会者或媒体发布端。
     * endpoint_id 为 0 时，当前来源信息被视为无效。
     */
    uint64_t endpoint_id = 0;

    /**
     * @brief 业务会话标识。
     *
     * 例如通话 ID、房间 ID 或会议 ID。
     */
    std::string session_id;

    /**
     * @brief 会话中的媒体流标识。
     *
     * 用于区分同一会话中的不同发布流。
     */
    std::string stream_id;

    /**
     * @brief 媒体轨道标识。
     *
     * 用于区分同一路媒体流中的音频轨道、视频轨道、
     * 屏幕共享轨道等。
     */
    TrackId track_id = 0;

     /**
     * @brief RTP 同步源标识。
     *
     * 用于标识该编码帧所属的 RTP SSRC。
     * 非 RTP 来源时可以为 0。
     */
    uint32_t ssrc = 0;
};

/**
 * @brief 编码帧发布事件。
 *
 * 包含帧的来源信息和编码后的媒体帧。
 *
 * frame 使用只读智能指针：
 * 1. 避免在多个订阅者之间复制大块帧数据；
 * 2. 防止订阅者修改其他模块正在使用的帧。
 */
struct EncodedFrameEvent
{
    EncodedFrameSource source;
    EncodedFrame::ConstPtr frame;

    /**
     * @brief 检查当前编码帧事件是否有效。
     *
     * 有效条件：
     * 1. endpoint_id 不为 0；
     * 2. frame 指针不为空；
     * 3. 编码帧自身通过有效性检查。
     *
     * @return true 事件有效。
     * @return false 事件无效，不应该继续发布或录制。
     *
     * @note 当前没有强制要求 session_id、stream_id、track_id
     *       和 ssrc 必须有效，具体约束由下游业务决定。
     */
    bool Valid() const noexcept
    {
        return source.endpoint_id != 0 && frame && frame->Valid();
    }
};

/**
 * @brief 编码帧订阅者接口。
 *
 * 录制、媒体转发、帧分析等模块可以实现该接口，
 * 然后注册到 EncodedFrameRouter 中接收编码帧。
 */
class IEncodedFrameSink
{
public:
    virtual ~IEncodedFrameSink() = default;

    // This method executes on a media worker and must never block.
    // Called synchronously by the publisher. Implementations should hand the
    // frame to their own processing context and return without blocking.
    virtual bool SubmitFrame(const EncodedFrameEvent& event) = 0;
};

/**
 * @brief 编码帧发布者接口。
 *
 * 上游编码器只依赖该接口发布帧，不需要关心后面有哪些订阅者，
 * 从而实现编码模块与录制、转发等业务模块解耦。
 */
class IEncodedFramePublisher
{
public:
    virtual ~IEncodedFramePublisher() = default;
    virtual size_t Publish(const EncodedFrameEvent& event) = 0;
};


/**
 * @brief 编码帧路由器。
 *
 * 管理编码帧订阅者，并将上游产生的 EncodedFrameEvent
 * 分发给当前处于活动状态的订阅者。
 *
 * 典型订阅者包括：
 * - 录制模块；
 * - SFU 转发模块；
 * - 实时翻译模块；
 * - 截图或视频分析模块；
 * - 编码帧统计模块。
 */
class EncodedFrameRouter final : public IEncodedFramePublisher
{
public:
    using SubscriptionId = uint64_t;

    SubscriptionId Subscribe(std::shared_ptr<IEncodedFrameSink> sink);
    void Unsubscribe(SubscriptionId id);
    size_t Publish(const EncodedFrameEvent& event) override;

private:
    struct Slot
    {
        explicit Slot(std::shared_ptr<IEncodedFrameSink> value)
            : sink(std::move(value))
        {
        }

        std::atomic<bool> active{true};
        std::weak_ptr<IEncodedFrameSink> sink;
    };

    std::mutex mutex_;
    std::unordered_map<SubscriptionId, std::shared_ptr<Slot>> slots_;
    std::atomic<SubscriptionId> next_id_{1};
};

} // namespace media

#endif /* _ENCODED_FRAME_ROUTER_H_ */
