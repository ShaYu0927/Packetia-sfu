#include "BufferWrite.h"
#include "Socket.h"
#include "SocketUtil.h"

BufferWirte::BufferWirte(int capacity)
    :max_queue_size(capacity)
{
}

bool BufferWirte::Append(std::shared_ptr<char> data, uint32_t size, uint32_t index)
{
    if(size < index)
    {
        return false;
    }

    if(size > max_queue_size)
    {
        return false;
    }

    Packet pkt = {data,size,index};
    buffer_.emplace(pkt);
    return true;
}

bool BufferWirte::Append(const char* data,uint32_t size,uint32_t index)
{
    if(size < index)
    {
        return false;
    }

    if(size > max_queue_size)
    {
        return false;
    }

    Packet pkt;
    pkt.data.reset(new char[size+512], std::default_delete<char[]>());
	memcpy(pkt.data.get(), data, size);
	pkt.size = size;
	pkt.writeIndex = index;
	buffer_.emplace(std::move(pkt));
    return true;
}

int  BufferWirte::Send(int socketfd,int timeOut)
{
    if(timeOut > 0)
    {
        SocketUtil::SetBlock(socketfd,timeOut);
    }

    int ret   = 0;
    int count = 0;

   while (!buffer_.empty()) {
        Packet &pkt = buffer_.front();
        int ret = send(socketfd, pkt.data.get() + pkt.writeIndex, pkt.size - pkt.writeIndex, 0);
        if (ret > 0) {
            pkt.writeIndex += ret;
            if (pkt.writeIndex == pkt.size) buffer_.pop();
        } else if (ret < 0) {
#if defined(__linux__) || defined(__linux__)
            if(errno == EINTR) continue;        // 信号中断重试
            if(errno == EAGAIN || errno == EWOULDBLOCK) break; // 等待下次写事件
#endif
            return -1; // 其他错误，关闭连接
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