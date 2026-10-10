#include "BufferRead.h"
#include "Socket.h"
#include <cstdint>
#include <cerrno>
#include <cstring>


const char BufferReader::kCRLF[] = "\r\n";

BufferReader::BufferReader(uint32_t initial_size, uint32_t max_buffered_bytes)
    : max_buffered_bytes_(max_buffered_bytes == 0
          ? KDefaultMaxBufferedBytes : max_buffered_bytes)
{
    buffer_.resize(std::min(std::max(initial_size, uint32_t{1}), max_buffered_bytes_));
}

BufferReader::~BufferReader()
{
}

uint32_t BufferReader::ReadableBytes() const
{
    return (uint32_t)(writer_index_ - reader_index_);
}

uint32_t BufferReader::WritableBytes() const
{
    return (uint32_t)(buffer_.size() - writer_index_);
}

char *BufferReader::Peek()
{
    return Begin() + reader_index_;
}

int BufferReader::Read(int sockfd, uint32_t max_read_bytes)
try
{
    const auto readable = ReadableBytes();
    if (readable >= max_buffered_bytes_)
    {
        errno = EMSGSIZE;
        return -1;
    }
    if (max_read_bytes == 0)
    {
        errno = EINVAL;
        return -1;
    }
    const auto requested = std::min({max_read_bytes, MAX_BYTES_PER_READ,
                                    max_buffered_bytes_ - readable});
    if (WritableBytes() < requested && reader_index_ != 0)
    {
        std::memmove(Begin(), Peek(), readable);
        reader_index_ = 0;
        writer_index_ = readable;
    }
    if (WritableBytes() < requested)
    {
        const auto needed = writer_index_ + requested;
        if (buffer_.capacity() < needed)
        {
            const auto grown = std::max(needed, buffer_.capacity() * 2);
            buffer_.reserve(std::min<size_t>(max_buffered_bytes_, grown));
        }
        buffer_.resize(needed);
    }
    int bytes_read = ::recv(sockfd, beginWrite(), requested, 0);
    if(bytes_read > 0) 
    {
		writer_index_ += bytes_read;
	}
    return bytes_read;
}
catch (const std::bad_alloc&)
{
    errno = ENOMEM;
    return -1;
}

uint32_t BufferReader::ReadAll(std::string &data)
{
    uint32_t size = ReadableBytes();
    if(size > 0)
    {
        data.append(Peek(), size);
        writer_index_ = 0;
		reader_index_ = 0;
    }
    return size;
}

uint32_t BufferReader::ReadUntilCrlf(std::string &data)
{
    const char* crlf = FindLastCrlf();
	if(crlf == nullptr)  {
		return 0;
	}

	uint32_t size = (uint32_t)(crlf - Peek() + 2);
	data.assign(Peek(), size);
	Retrieve(size);
	return size;
}

uint32_t ReadUint32BE(char *data)
{
    uint8_t* p = (uint8_t*)data;
    return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

uint32_t ReadUint32LE(char *data)
{
    uint8_t* p = (uint8_t*)data;
    return (p[3] << 24) | (p[2] << 16) | (p[1] << 8) | p[0];
}

uint32_t ReadUint24BE(char *data)
{
    uint8_t* p = (uint8_t*)data;
    return (p[0] << 16) | (p[1] << 8) | p[2];
}

uint32_t ReadUint24LE(char *data)
{
    uint8_t* p = (uint8_t*)data;
    return (p[2] << 16) | (p[1] << 8) | p[0];
}

uint16_t ReadUint16BE(const uint8_t*data)
{
    uint8_t* p = (uint8_t*)data;
    return (p[0] << 8) | p[1];
}

uint16_t ReadUint16LE(char *data)
{
    uint8_t* p = (uint8_t*)data;
    return (p[1] << 8) | p[0];
}
