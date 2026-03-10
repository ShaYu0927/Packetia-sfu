#ifndef _BUFFERREAD_H_
#define _BUFFERREAD_H_


#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

uint32_t ReadUint32BE(char* data);
uint32_t ReadUint32LE(char* data);
uint32_t ReadUint24BE(char* data);
uint32_t ReadUint24LE(char* data);
uint16_t ReadUint16BE(char* data);
uint16_t ReadUint16LE(char* data);

class BufferReader
{
public:
    BufferReader(uint32_t initial_size = 2048);
    virtual ~BufferReader();

    uint32_t ReadableBytes() const;
    uint32_t WritableBytes() const;

    char* Peek(); 

    uint32_t Size() const
    {
        return (uint32_t)buffer_.size();
    }

    const char* Peek() const  //获取当前可读数据的起始位置
	{ return Begin() + reader_index_; }
    
    const char* FindFirstCrlf() const 
    {    
		const char* crlf = std::search(Peek(), BeginWrite(), kCRLF, kCRLF+2);
		return crlf == BeginWrite() ? nullptr : crlf;
	}

    const char* FindLastCrlf() const 
    {    
		const char* crlf = std::find_end(Peek(), BeginWrite(), kCRLF, kCRLF+2);
		return crlf == BeginWrite() ? nullptr : crlf;
	}
    const char* FindLastCrlfCrlf() const 
    {
		char crlfCrlf[] = "\r\n\r\n";
		const char* crlf = std::find_end(Peek(), BeginWrite(), crlfCrlf, crlfCrlf + 4);
		return crlf == BeginWrite() ? nullptr : crlf;
	}

    void RetrieveAll()  
    { 
		writer_index_ = 0; 
		reader_index_ = 0; 
	}

    void Retrieve(size_t len) 
    {
		if (len <= ReadableBytes()) 
		{
			reader_index_ += len;
			if(reader_index_ == writer_index_) 
			{
				reader_index_ = 0;
				writer_index_ = 0;
			}
		}
		else 
		{
			RetrieveAll();
		}
	}

    void RetrieveUntil(const char* end)
	{ Retrieve(end - Peek()); }

    int Read(int sockfd);
	uint32_t ReadAll(std::string& data);
	uint32_t ReadUntilCrlf(std::string& data);
private:

    char* Begin()
    {
        return &*buffer_.begin();
    }

    const char* Begin() const
    {
        return &*buffer_.begin();
    }

    char* beginWrite()
	{ return Begin() + writer_index_; }

	const char* BeginWrite() const
	{ return Begin() + writer_index_; }


    std::vector<char> buffer_;
	size_t reader_index_ = 0;
	size_t writer_index_ = 0;

	static const char kCRLF[];
	static const uint32_t MAX_BYTES_PER_READ = 4096;
	static const uint32_t MAX_BUFFER_SIZE = 1024 * 100000;
};


#endif