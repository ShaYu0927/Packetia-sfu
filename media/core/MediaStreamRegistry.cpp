#include "MediaStreamRegistry.h"

namespace media
{
MediaStreamRegistry& MediaStreamRegistry::Instance()
{
    static MediaStreamRegistry registry;
    return registry;
}

bool MediaStreamRegistry::Register(const MediaStream::Ptr& stream)
{
    if (!stream || stream->GetName().empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    // 统一按 registry -> stream 加锁；同一对象不能同时归属两个注册表。
    std::lock_guard<std::mutex> stream_lock(stream->mutex_);
    if (names_.count(stream->GetName()) || streams_.count(stream->GetId()) || stream->registry_ ||
        stream->state_ == MediaStream::State::Closed || stream->tracks_.empty()) return false;
    stream->publisher_ = publisher_;
    streams_.emplace(stream->GetId(), stream);
    names_.emplace(stream->GetName(), stream);
    stream->registry_ = this;
    return true;
}

MediaStream::Ptr MediaStreamRegistry::FindById(uint32_t id) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = streams_.find(id);
    return it == streams_.end() ? nullptr : it->second;
}

MediaStream::Ptr MediaStreamRegistry::FindByName(const std::string& name) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = names_.find(name);
    return it == names_.end() ? nullptr : it->second;
}

bool MediaStreamRegistry::Remove(const MediaStream::Ptr& expected)
{
    if (!expected) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = streams_.find(expected->GetId());
    if (it == streams_.end() || it->second != expected) return false;
    names_.erase(expected->GetName());
    streams_.erase(it);
    expected->Close();
    return true;
}

std::size_t MediaStreamRegistry::Size() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return streams_.size();
}

void MediaStreamRegistry::SetFramePublisher(std::shared_ptr<IEncodedFramePublisher> publisher)
{
    std::lock_guard<std::mutex> lock(mutex_);
    publisher_ = std::move(publisher);
    for (const auto& entry : streams_) entry.second->SetFramePublisher(publisher_);
}

void MediaStreamRegistry::Clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : streams_) entry.second->Close();
    streams_.clear();
    names_.clear();
}
}
