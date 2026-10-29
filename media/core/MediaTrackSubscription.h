#ifndef PACKETIA_MEDIA_CORE_MEDIA_TRACK_SUBSCRIPTION_H_
#define PACKETIA_MEDIA_CORE_MEDIA_TRACK_SUBSCRIPTION_H_

#include "MediaTypes.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace media
{
enum class MediaTrackState
{
    Open,
    Paused,
    Closed
};

// 一条实际消费关系，不是新的源轨道。持有者释放句柄即解除关系。
// 热路径只检查原子状态；管理器不持有消费者或协议对象的强引用。
class MediaTrackSubscription
{
public:
    using Ptr = std::shared_ptr<MediaTrackSubscription>;

    uint64_t Id() const noexcept
    {
        return id_;
    }

    MediaTrackRef Track() const noexcept
    {
        return track_;
    }

    const std::string& Name() const noexcept
    {
        return name_;
    }

    bool Closed() const noexcept
    {
        return closed_.load() || source_->load() == MediaTrackState::Closed;
    }

    bool Paused() const noexcept
    {
        return paused_.load();
    }

    bool CanDeliver() const noexcept
    {
        return !closed_.load() && !paused_.load() && source_->load() == MediaTrackState::Open;
    }

    // 不等待已开始的发送/回调，也不排空消费者自己的队列。
    void Close() noexcept
    {
        closed_.store(true);
    }

private:
    friend class MediaStreamRegistry;

    MediaTrackSubscription(uint64_t id, MediaTrackRef track, std::string name,
        std::shared_ptr<std::atomic<MediaTrackState>> source, std::function<void()> on_close)
        : id_(id), track_(track), name_(std::move(name)), source_(std::move(source)),
          on_close_(std::move(on_close))
    {
    }

    bool NotifyClosed() noexcept
    {
        if (closed_.exchange(true))
        {
            return false;
        }
        if (on_close_)
        {
            try
            {
                on_close_();
            }
            catch (...)
            {
                // 一个消费者不能阻断其余关系清理。
            }
        }
        return true;
    }

    const uint64_t id_;
    const MediaTrackRef track_;
    const std::string name_;
    const std::shared_ptr<std::atomic<MediaTrackState>> source_;
    const std::function<void()> on_close_;
    std::atomic<bool> closed_{false};
    std::atomic<bool> paused_{false};
};
}
#endif
