//
// Created by roots on 2024/9/11.
//

#ifndef FFMPEGAAC_CHANNEL_H
#define FFMPEGAAC_CHANNEL_H

#include <functional>
#include <memory>

enum EventType
{
    EVENT_NONE = 0,
    EVENT_IN = 1,
    EVENT_PRI = 2,
    EVENT_OUT = 4,
    EVENT_ERROR = 8,
    EVENT_HUP= 16,
    EVENT_RDHUP  = 8192 //远程关闭事件
};

class Channel {
public:
    typedef srd::function<void()> EventCallback;
    Channel() = delete;

    Channel(int fd)
    : socket_(fd), events_(0)
    {
    }

    virtual ~Channel()
    {
    }

    void setReadCallback(EventCallback cb)
    {
        readCallback_ = cb;
    }
    void setWriteCallback(EventCallback cb)
    {
        writeCallback_ = cb;
    }
    void setErrorCallback(EventCallback cb)
    {
        errorCallback_ = cb;
    }

    int fd() const
    {
        return socket_;
    }

    void setEvents(int events)
    {
        events_ = events;
    }
    int GetEvents() const
    {
        return events_;
    }

    void HandleEvent()
    {
        if (events_ & EVENT_IN)
        {
            readCallback_();
        }
        if (events_ & EVENT_OUT)
        {
            writeCallback_();
        }
        if (events_ & EVENT_ERROR)
        {
            errorCallback_();
        }
    }

    void EnableReading() {
        events_ |= EVENT_IN;
    }

    void EnableWriting() {
        events_ |= EVENT_OUT;
    }

    void DisableReading() {
        events_ &= ~EVENT_IN;
    }

    void DisableWriting()
    { events_ &= ~EVENT_OUT; }
private:
    EventCallback readCallback_ = []{};
    EventCallback writeCallback_ = []{};
    EventCallback errorCallback_ = []{};

    int socket_;
    int events_;
};


#endif //FFMPEGAAC_CHANNEL_H
