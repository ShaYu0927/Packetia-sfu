#pragma once

#include <cstdint>
#include <string>

namespace service {

// Random-access byte stream required by MOV/MP4 muxers. Implementations use
// libmov-compatible return values: zero on success and a negative error code
// on failure. Instances are owned by one writer and are not thread-safe.
class ISeekableFile {
public:
    virtual ~ISeekableFile() = default;

    virtual int Read(void* data, uint64_t bytes) = 0;
    virtual int Write(const void* data, uint64_t bytes) = 0;
    // Nonnegative offsets are absolute; negative offsets are relative to EOF.
    virtual int Seek(int64_t offset) = 0;
    virtual int64_t Tell() = 0;
    virtual int64_t Size() = 0;
    virtual int Flush() = 0;
    virtual int Close() = 0;

    virtual bool IsOpen() const = 0;
    virtual std::string Error() const = 0;
};

}
