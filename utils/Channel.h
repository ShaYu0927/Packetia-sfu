#ifndef _CHANNEL_H_
#define _CHANNEL_H_

#include <functional>
#include <memory>
#include "Socket.h"

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
    typedef std::function<void()> EventCallback;

    Channel() = delete;

    Channel(SOCKET sockfd) 
		: sockfd_(sockfd)
	{

	}

    void SetReadCallback(const EventCallback& cb)
    {
        readCallback_ = cb;
    }

    void SetWriteCallback(const EventCallback& cb)
    {
        writeCallback_ = cb;
    }

    void SetErrorCallback(const EventCallback& cb)
    {
        errorCallback_ = cb;
    }

    void SetCloseCallback(const EventCallback& cb)
    {
        close_callback_ = cb;
    }

    int GetSocket() const
    {
        return sockfd_;
    }

    void SetEvents(int events)
    {
        events_ = events;
    }

    int GetEvents() const
    {
        return events_;
    }   

    void EnableReading() 
	{ events_ |= EVENT_IN; }

	void EnableWriting() 
	{ events_ |= EVENT_OUT; }
    
	void DisableReading() 
	{ events_ &= ~EVENT_IN; }
    
	void DisableWriting() 
	{ events_ &= ~EVENT_OUT; }

    bool IsNoneEvent() const { return events_ == EVENT_NONE; }
	bool IsWriting() const { return (events_ & EVENT_OUT) != 0; }
	bool IsReading() const { return (events_ & EVENT_IN) != 0; }

    void HandleEvent(int events)
    {
        if(events & EVENT_IN)
        {
            readCallback_();
        }

        if(events & EVENT_OUT)
        {
            writeCallback_();
        }

        if(events & EVENT_HUP)
        {
            close_callback_();
			return ;
        }

        if(events & EVENT_ERROR)
        {
            errorCallback_();
        }
    }
private:
    SOCKET sockfd_;
    int events_;
    EventCallback readCallback_ = []{};
    EventCallback writeCallback_ = []{};
    EventCallback errorCallback_ = []{};
    EventCallback close_callback_ = []{};
};

typedef std::shared_ptr<Channel> ChannelPtr;

#endif