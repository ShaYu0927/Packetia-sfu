#include "EncodedFrameRouter.h"
#include "MediaStreamRegistry.h"

namespace media
{
// 单独编译注册表绑定，使旧的独立帧路由使用者不必链接全局目录实现。
EncodedFrameRouter::SubscriptionId EncodedFrameRouter::Subscribe(
    std::shared_ptr<IEncodedFrameSink> sink, MediaTrackRef track, MediaStreamRegistry& registry,
    std::string name, std::function<void()> on_close)
{
    if (!sink || !track.Valid()) return 0;
    auto consumer = registry.AttachConsumer(track, std::move(name), std::move(on_close));
    if (!consumer) return 0;
    return SubscribeImpl(std::move(sink), EncodedFrameFilter::ForTrack(track), std::move(consumer));
}
}
