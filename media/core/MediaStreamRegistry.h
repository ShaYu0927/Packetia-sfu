#ifndef PACKETIA_MEDIA_STREAM_REGISTRY_H_
#define PACKETIA_MEDIA_STREAM_REGISTRY_H_

#include "MediaStream.h"

namespace media
{
// 注册表拥有活动流；接入层拥有发布生命周期，先停传输/Endpoint，再移除注册。
// 可创建独立实例；Instance 用于当前单节点服务的进程内共享。
class MediaStreamRegistry
{
public:
    ~MediaStreamRegistry() { Clear(); }
    static MediaStreamRegistry& Instance();
    bool Register(const MediaStream::Ptr& stream);
    MediaStream::Ptr FindById(uint32_t id) const;
    MediaStream::Ptr FindByName(const std::string& name) const;
    bool Remove(const MediaStream::Ptr& expected);
    std::size_t Size() const;
    void SetFramePublisher(std::shared_ptr<IEncodedFramePublisher> publisher);
    void Clear();

private:
    mutable std::mutex mutex_;
    std::unordered_map<uint32_t, MediaStream::Ptr> streams_;
    std::unordered_map<std::string, MediaStream::Ptr> names_;
    std::shared_ptr<IEncodedFramePublisher> publisher_;
};
}

#endif
