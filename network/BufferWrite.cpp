#include "BufferWrite.h"

bool BufferWirte::Append(std::shared_ptr<char> data,uint32_t size,uint32_t index)
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
        SocketUtil::SetBlock(socketfd, timeOut); 
    }

    int ret   = 0;
    int count = 0;

    do
    {
        /* code */

    if(buffer_.size() == 0)
    {
        return 0;
    }

    count -= 1;
    Packet pkt = buffer_.front(); //获取缓冲区前头
    ret = send(socketfd,pkt.data.get() + pkt.writeIndex,pkt.size - pkt.writeIndex,0);
    if(ret > 0)
    {
        pkt.writeIndex += ret;
        if(pkt.size == pkt.writeIndex) //发送完毕
        {
            count += 1;
            buffer_.pop();
        }
        else if(ret < 0)
        {
#if defined(__linux) || defined(__linux__)
		if (errno == EINTR || errno == EAGAIN) 
#elif defined(WIN32) || defined(_WIN32)
			int error = WSAGetLastError();
			if (error == WSAEWOULDBLOCK || error == WSAEINPROGRESS || error == 0)
#endif
			{
				ret = 0;
			}
        }
    }

    } while (count > 0);
    
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