#ifndef _BUFFERWRITE_H_
#define _BUFFERWRITE_H_

#include <memory>
#include <queue>
#include <cstdint>
#include <cstddef>


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

    static constexpr std::size_t KDefaultMaxQueuedBytes = 4 * 1024 * 1024;

    explicit BufferWirte(std::size_t max_queued_bytes = KDefaultMaxQueuedBytes);
	~BufferWirte() {}
    bool IsEmpty() const
    {
        return buffer_.empty();
    }

    bool IsFull() const
    {
        return queued_bytes_ >= max_queued_bytes_;
    }

    std::size_t Size() const
    {
        return buffer_.size();
    }

    std::size_t QueuedBytes() const { return queued_bytes_; }
    std::size_t CapacityBytes() const { return max_queued_bytes_; }

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

    std::size_t max_queued_bytes_ = KDefaultMaxQueuedBytes;
    std::size_t queued_bytes_ = 0;
    std::queue<Packet> buffer_;
};



#endif
