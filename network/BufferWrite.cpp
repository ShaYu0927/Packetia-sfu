#include "BufferWrite.h"
#include "SocketUtil.h"
#include <cstring>
#include <utility>
#include "Socket.h"

BufferWirte::BufferWirte(std::size_t max_queued_bytes)
    : max_queued_bytes_(max_queued_bytes == 0
          ? KDefaultMaxQueuedBytes
          : max_queued_bytes)
{
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
        return false;
    }

    Packet pkt = {std::move(data), size, index};
    buffer_.emplace(std::move(pkt));
    queued_bytes_ += remaining;
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
        return false;
    }

    Packet pkt;
    pkt.data.reset(new char[size], std::default_delete<char[]>());
	memcpy(pkt.data.get(), data, size);
	pkt.size = size;
	pkt.writeIndex = index;
	buffer_.emplace(std::move(pkt));
    queued_bytes_ += remaining;
    return true;
}

int  BufferWirte::Send(int socketfd,int timeOut)
{
    if(timeOut > 0)
    {
        SocketUtil::SetBlock(socketfd,timeOut);
    }

   while (!buffer_.empty()) 
   {
        Packet &pkt = buffer_.front();
#ifdef MSG_NOSIGNAL
        constexpr int flags = MSG_NOSIGNAL;
#else
        constexpr int flags = 0;
#endif
        const int ret = send(socketfd, pkt.data.get() + pkt.writeIndex, pkt.size - pkt.writeIndex, flags);
        if (ret > 0) 
        {
            pkt.writeIndex += static_cast<uint32_t>(ret);
            queued_bytes_ -= static_cast<std::size_t>(ret);
            if (pkt.writeIndex == pkt.size)
            {
                buffer_.pop();
            }
        } 
        else if (ret == 0)
        {
            return -1;
        }
        else if (ret < 0) 
        {
#if defined(__linux__) || defined(__linux__)
            if(errno == EINTR) continue;        
            if(errno == EAGAIN || errno == EWOULDBLOCK) break; 
#endif
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
