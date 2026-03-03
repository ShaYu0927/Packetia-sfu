#ifndef _BUFFERWRITE_H_
#define _BUFFERWRITE_H_

#include <memory>
#include <queue>
#include <cstdint>
#


void WriteUint32BE(char* p, uint32_t value);
void WriteUint32LE(char* p, uint32_t value);
void WriteUint24BE(char* p, uint32_t value);
void WriteUint24LE(char* p, uint32_t value);
void WriteUint16BE(char* p, uint32_t value);
void WriteUint16LE(char* p, uint32_t value);

class BufferWirte : public std::enable_shared_from_this<BufferWirte>
{
public:
    using Ptr = std::shared_ptr<BufferWirte>;
    BufferWirte(int capacity = KMaxQueueLength);
	~BufferWirte() {}
    bool IsEmpty() const
    {
        return buffer_.empty();
    }

    bool IsFull() const
    {
        return buffer_.size() == max_queue_size;
    }

    uint32_t Size()
    {
        return (uint32_t)buffer_.size();
    }

    bool Append(std::shared_ptr<char> data,uint32_t size,uint32_t index = 0);
    bool Append(const char* data,uint32_t size,uint32_t index = 0);
    int  Send(int socketfd,int timeOut = 0);
private:
    typedef struct 
    {
        std::shared_ptr<char> data;
        uint32_t size;
        uint32_t writeIndex;
    }Packet;

    int max_queue_size = 0;
    std::queue<Packet> buffer_;
    static constexpr int KMaxQueueLength = 0;
};



#endif