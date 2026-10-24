#include "BufferWrite.h"
#include "MemoryPool.h"
#include "SocketUtil.h"
#include <cstring>
#include <utility>
#include "Socket.h"

BufferWirte::BufferWirte(std::size_t max_queued_bytes)
    : max_queued_bytes_(max_queued_bytes == 0
          ? KDefaultMaxQueuedBytes
          : max_queued_bytes), metrics_(monitoring::RegisterSendQueue("tcp"))
{
}

BufferWirte::~BufferWirte()
{
    if (!buffer_.empty())
        monitoring::RecordSend(monitoring::Transport::Tcp, monitoring::SendEvent::Abandoned, 0, buffer_.size());
    if (metrics_) metrics_->Set(0, 0, 0);
    while (!buffer_.empty())
    {
        if (buffer_.front().trace)
            media_latency::Count(media_latency::Counter::TcpAbandoned);
        buffer_.pop();
    }
}

void BufferWirte::UpdateMetrics()
{
    if (metrics_) metrics_->Set(queued_bytes_, buffer_.size(), buffer_.empty() ? 0 : buffer_.front().enqueue_ns);
}

bool BufferWirte::Append(std::shared_ptr<char> data, uint32_t size, uint32_t index)
{
    if (!data || size == 0 || index >= size)
    {
        return false;
    }

    const std::size_t remaining = static_cast<std::size_t>(size - index);
    if (remaining > max_queued_bytes_ - queued_bytes_)
    {
        monitoring::RecordSend(monitoring::Transport::Tcp, monitoring::SendEvent::Rejected);
        return false;
    }

    Packet pkt = {std::move(data), size, index, media_latency::CurrentSend(), monitoring::NowNs()};
    buffer_.emplace(std::move(pkt));
    queued_bytes_ += remaining;
    UpdateMetrics();
    return true;
}

bool BufferWirte::Append(const char* data,uint32_t size,uint32_t index)
{
    if (!data || size == 0 || index >= size)
    {
        return false;
    }

    const std::size_t remaining = static_cast<std::size_t>(size - index);
    if (remaining > max_queued_bytes_ - queued_bytes_)
    {
        monitoring::RecordSend(monitoring::Transport::Tcp, monitoring::SendEvent::Rejected);
        return false;
    }

    Packet pkt;
    auto allocation = common::MemoryPool::Default().TryAllocate(size);
    if (!allocation)
    {
        monitoring::RecordSend(monitoring::Transport::Tcp, monitoring::SendEvent::Rejected);
        return false;
    }
    std::shared_ptr<uint8_t[]> storage(std::move(allocation));
    pkt.data = std::shared_ptr<char>(storage, reinterpret_cast<char*>(storage.get()));
	memcpy(pkt.data.get(), data, size);
	pkt.size = size;
	pkt.writeIndex = index;
    pkt.trace = media_latency::CurrentSend();
    pkt.enqueue_ns = monitoring::NowNs();
	buffer_.emplace(std::move(pkt));
    queued_bytes_ += remaining;
    UpdateMetrics();
    return true;
}

int  BufferWirte::Send(int socketfd,int timeOut)
{
    struct PublishQueue { BufferWirte& buffer; ~PublishQueue() { buffer.UpdateMetrics(); } } publish{*this};
    if(timeOut > 0)
    {
        if (!SocketUtil::SetBlock(socketfd,timeOut))
        {
            monitoring::RecordSend(monitoring::Transport::Tcp, monitoring::SendEvent::Failed);
            return -1;
        }
    }

   while (!buffer_.empty()) 
   {
        Packet &pkt = buffer_.front();
#ifdef MSG_NOSIGNAL
        constexpr int flags = MSG_NOSIGNAL;
#else
        constexpr int flags = 0;
#endif
        const auto ret = send(socketfd, pkt.data.get() + pkt.writeIndex, pkt.size - pkt.writeIndex, flags);
        if (ret > 0) 
        {
            pkt.writeIndex += static_cast<uint32_t>(ret);
            queued_bytes_ -= static_cast<std::size_t>(ret);
            monitoring::RecordSend(monitoring::Transport::Tcp, monitoring::SendEvent::Sent,
                                   static_cast<uint64_t>(ret), pkt.writeIndex == pkt.size ? 1 : 0);
            if (pkt.writeIndex == pkt.size)
            {
                if (pkt.trace)
                    media_latency::SocketSent(pkt.trace, true, media_latency::NowNs());
                buffer_.pop();
            }
        } 
        else if (ret == 0)
        {
            monitoring::RecordSend(monitoring::Transport::Tcp, monitoring::SendEvent::Failed);
            return -1;
        }
        else if (ret < 0) 
        {
            if(errno == EINTR) continue;        
            if(errno == EAGAIN || errno == EWOULDBLOCK)
            {
                monitoring::RecordSend(monitoring::Transport::Tcp, monitoring::SendEvent::Blocked);
                break;
            }
            monitoring::RecordSend(monitoring::Transport::Tcp, monitoring::SendEvent::Failed);
            return -1;
        }
    }
    return 0;
}


void WriteUint32BE(char* p, uint32_t value)
{
    p[0] = value >> 24;
    p[1] = value >> 16;
    p[2] = value >> 8;
	p[3] = value & 0xff;
}
void WriteUint32LE(char* p, uint32_t value)
{
    p[0] = value & 0xff;
	p[1] = value >> 8;
	p[2] = value >> 16;
	p[3] = value >> 24;
}
void WriteUint24BE(char* p, uint32_t value)
{
    p[0] = value >> 16;
	p[1] = value >> 8;
	p[2] = value & 0xff;
}
void WriteUint24LE(char* p, uint32_t value)
{
    p[0] = value & 0xff;
	p[1] = value >> 8;
	p[2] = value >> 16;
}
void WriteUint16BE(char* p, uint32_t value)
{
    p[0] = value >> 8;
	p[1] = value & 0xff;
}
void WriteUint16LE(char* p, uint32_t value)
{
    p[0] = value & 0xff;
	p[1] = value >> 8;
}
