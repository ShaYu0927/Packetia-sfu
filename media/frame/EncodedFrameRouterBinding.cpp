#include "EncodedFrameRouter.h"
#include "MediaStreamRegistry.h"

namespace media
{
// 单独编译注册表绑定，使旧的独立帧路由使用者不必链接全局目录实现。
EncodedFrameRouter::SubscriptionId EncodedFrameRouter::Subscribe(
    std::shared_ptr<IEncodedFrameSink> sink, MediaTrackRef track, MediaStreamRegistry& registry,
    std::string name, std::function<void()> on_close)
{
    return TrySubscribe(std::move(sink), track, registry, std::move(name), std::move(on_close)).id;
}

EncodedFrameRouter::SubscribeResult EncodedFrameRouter::TrySubscribe(
    std::shared_ptr<IEncodedFrameSink> sink, MediaTrackRef track, MediaStreamRegistry& registry,
    std::string name, std::function<void()> on_close)
{
    if (!sink || !track.Valid() || name.empty()) return {SubscribeStatus::Invalid, 0};
    auto consumer = registry.AttachConsumer(track, std::move(name), std::move(on_close));
    if (!consumer) return {SubscribeStatus::SourceRejected, 0};
    try
    {
        const auto result = SubscribeImpl(std::move(sink), EncodedFrameFilter::ForTrack(track), consumer);
        if (!result) consumer->Close();
        return result;
    }
    catch (...)
    {
        // 准入失败时先封住关系；注册表的并发快照可能仍持有句柄。
        consumer->Close();
        throw;
    }
}
}
