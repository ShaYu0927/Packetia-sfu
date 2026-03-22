#include "BufferRead.h"
#include "Socket.h"
#include <cstdint>


const char BufferReader::kCRLF[] = "\r\n";

BufferReader::BufferReader(uint32_t initial_size)
{
    buffer_.resize(initial_size);
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

int BufferReader::Read(int sockfd)
{
    uint32_t size = WritableBytes();
    if (size == 0) 
    {
        buffer_.resize(buffer_.size() * 2);
    }
    if(size < MAX_BYTES_PER_READ)
    {
        uint32_t bufferReaderSize = (uint32_t)buffer_.size();
		if(bufferReaderSize > MAX_BUFFER_SIZE) 
        {
			return 0; 
		}
        buffer_.resize(bufferReaderSize + MAX_BYTES_PER_READ);
    }
    int bytes_read = ::recv(sockfd, beginWrite(), MAX_BYTES_PER_READ, 0);
    if(bytes_read > 0) 
    {
		writer_index_ += bytes_read;
	}
    return bytes_read;
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
