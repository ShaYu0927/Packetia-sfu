#ifndef _EVENT_BUS_H_
#define _EVENT_BUS_H_


#include <atomic>
#include <string>
#include <stdint.h>
#include <functional>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <atomic>
#include <utility>
#

#include "stats/MediaStatsTypes.h"
#include "coreEvent.h"

namespace media
{

enum class MediaObjectType
{
    Unknown = 0,
    Worker,
    Session,
    Transport,
    Producer,
    Consumer,
    ReceiverTrack,
    SenderTrack
};

class MediaObject
{
public:
    MediaObject(MediaObjectType type, const std::string& id)
        : type_(type),
          id_(id),
          closed_(false)
    {
    }

    virtual ~MediaObject()
    {
    }

    MediaObjectType Type() const { return type_; }

    const std::string& Id() const { return id_; }

    bool IsClosed() const { return closed_.load(); }

    void Close();

protected:
    virtual void OnClose()
    {
    }

private:
    MediaObjectType type_ = MediaObjectType::Unknown;
    std::string id_;
    std::atomic<bool> closed_;
};

class MediaProducer : public MediaObject
{
public:
    explicit MediaProducer(const std::string& id)
        : MediaObject(MediaObjectType::Producer, id)
    {
    }

    virtual bool InputRtpPacket(const MediaTrackStats& packet) = 0;
};

class MediaConsumer : public MediaObject
{
public:
    explicit MediaConsumer(const std::string& id)
        : MediaObject(MediaObjectType::Consumer, id)
    {
    }

    virtual bool SendRtpPacket(const MediaTrackStats& packet) = 0;
};

class EventBus
{
public:
    using HandlerId = uint64_t;
    using RawCallback = std::function<void(const MediaEvent&)>;

public:
    class Subscription
    {
    public:
        Subscription()
            : bus_(nullptr),
              type_(MediaEventType::Unknown),
              id_(0)
        {
        }

        Subscription(EventBus* bus, MediaEventType type, HandlerId id)
            : bus_(bus),
              type_(type),
              id_(id)
        {
        }

        ~Subscription()
        {
            Reset();
        }

        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;

        Subscription(Subscription&& other) noexcept
            : bus_(other.bus_),
              type_(other.type_),
              id_(other.id_)
        {
            other.bus_ = nullptr;
            other.id_ = 0;
            other.type_ = MediaEventType::Unknown;
        }

        Subscription& operator=(Subscription&& other) noexcept
        {
            if (this != &other)
            {
                Reset();

                bus_ = other.bus_;
                type_ = other.type_;
                id_ = other.id_;

                other.bus_ = nullptr;
                other.id_ = 0;
                other.type_ = MediaEventType::Unknown;
            }

            return *this;
        }

        void Reset()
        {
            if (bus_ && id_ != 0)
            {
                bus_->Unsubscribe(type_, id_);
                bus_ = nullptr;
                id_ = 0;
                type_ = MediaEventType::Unknown;
            }
        }

        bool Valid() const
        {
            return bus_ != nullptr && id_ != 0;
        }

    private:
        EventBus* bus_;
        MediaEventType type_;
        HandlerId id_;
    };

public:
    EventBus()
        : next_id_(1)
    {
    }

    template <typename EventT>
    Subscription Subscribe(std::function<void(const EventT&)> callback)
    {
        if (!callback)
        {
            return Subscription();
        }

        MediaEventType type = EventT::StaticType();

        RawCallback wrapper = [callback](const MediaEvent& event) {
            const EventT* typed_event = dynamic_cast<const EventT*>(&event);
            if (!typed_event)
            {
                return;
            }

            callback(*typed_event);
        };

        HandlerId id = SubscribeRaw(type, wrapper);
        return Subscription(this, type, id);
    }

    size_t Publish(const MediaEvent& event)
    {
        std::vector<RawCallback> callbacks;

        {
            std::lock_guard<std::mutex> lock(mutex_);

            auto it = handlers_.find(event.Type());
            if (it == handlers_.end())
            {
                return 0;
            }

            callbacks.reserve(it->second.size());

            for (const auto& slot : it->second)
            {
                callbacks.push_back(slot.callback);
            }
        }

        for (const auto& cb : callbacks)
        {
            cb(event);
        }

        return callbacks.size();
    }

    template <typename EventT, typename... Args>
    size_t EmplacePublish(Args&&... args)
    {
        EventT event(std::forward<Args>(args)...);
        return Publish(event);
    }

private:
    struct HandlerSlot
    {
        HandlerId id = 0;
        RawCallback callback;
    };

    struct EventTypeHash
    {
        size_t operator()(MediaEventType type) const
        {
            return static_cast<size_t>(type);
        }
    };

private:
    HandlerId SubscribeRaw(MediaEventType type, RawCallback callback)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        HandlerId id = next_id_++;

        HandlerSlot slot;
        slot.id = id;
        slot.callback = callback;

        handlers_[type].push_back(slot);

        return id;
    }

    void Unsubscribe(MediaEventType type, HandlerId id)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = handlers_.find(type);
        if (it == handlers_.end())
        {
            return;
        }

        auto& vec = it->second;

        for (auto iter = vec.begin(); iter != vec.end(); ++iter)
        {
            if (iter->id == id)
            {
                vec.erase(iter);
                break;
            }
        }

        if (vec.empty())
        {
            handlers_.erase(it);
        }
    }

private:
    std::mutex mutex_;
    std::atomic<HandlerId> next_id_;

    std::unordered_map<
        MediaEventType,
        std::vector<HandlerSlot>,
        EventTypeHash
    > handlers_;
};

}


#endif /* _EVENT_BUS_H_ */
