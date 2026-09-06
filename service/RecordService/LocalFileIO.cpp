#if !defined(_WIN32) && !defined(_FILE_OFFSET_BITS)
#define _FILE_OFFSET_BITS 64
#endif

#include "LocalFileIO.h"
#include <cerrno>
#include <cstring>
#include <limits>
#ifndef _WIN32
#include <sys/types.h>
#endif

namespace service {

LocalFileIO::~LocalFileIO() { Close(); }

int LocalFileIO::Fail(int code) {
    error_code_ = code > 0 ? code : EIO;
    return -error_code_;
}

std::string LocalFileIO::Error() const {
    return error_code_ ? std::strerror(error_code_) : "";
}

int LocalFileIO::Open(const std::string& path, OpenMode mode) {
    if (file_) return Fail(EBUSY);
    error_code_ = 0;
    if (path.empty() || path.find('\0') != std::string::npos) return Fail(EINVAL);
    errno = 0;
    file_ = std::fopen(path.c_str(), mode == OpenMode::Create ? "wb+" : "rb");
    if (!file_) return Fail(errno);
    writable_ = mode == OpenMode::Create;
    direction_ = Direction::None;
    return 0;
}

int LocalFileIO::Prepare(Direction direction) {
    // C update streams require positioning between reads and writes.
    if (direction_ != Direction::None && direction_ != direction) {
        const auto position = Tell();
        if (position < 0) return static_cast<int>(position);
        const auto result = Seek(position);
        if (result < 0) return result;
    }
    direction_ = direction;
    return 0;
}

int LocalFileIO::Read(void* data, uint64_t bytes) {
    if (!file_) return Fail(EBADF);
    if (bytes > std::numeric_limits<size_t>::max() || (!data && bytes)) return Fail(EINVAL);
    if (!bytes) return 0;
    const auto result = Prepare(Direction::Read);
    if (result < 0) return result;
    errno = 0;
    const auto count = std::fread(data, 1, static_cast<size_t>(bytes), file_);
    // libmov requests exact reads; EOF and all other short reads are errors.
    return count == bytes ? 0 : Fail(errno);
}

int LocalFileIO::Write(const void* data, uint64_t bytes) {
    if (!file_ || !writable_) return Fail(EBADF);
    if (bytes > std::numeric_limits<size_t>::max() || (!data && bytes)) return Fail(EINVAL);
    if (!bytes) return 0;
    const auto result = Prepare(Direction::Write);
    if (result < 0) return result;
    errno = 0;
    const auto count = std::fwrite(data, 1, static_cast<size_t>(bytes), file_);
    return count == bytes ? 0 : Fail(errno);
}

int LocalFileIO::Seek(int64_t offset) {
    if (!file_) return Fail(EBADF);
    errno = 0;
#if defined(__MINGW32__) && !defined(__MINGW64_VERSION_MAJOR)
    const int result = ::fseeko64(file_, offset, offset < 0 ? SEEK_END : SEEK_SET);
#elif defined(_WIN32)
    const int result = _fseeki64(file_, offset, offset < 0 ? SEEK_END : SEEK_SET);
#else
    static_assert(sizeof(off_t) >= sizeof(int64_t), "LocalFileIO requires 64-bit offsets");
    const int result = ::fseeko(file_, static_cast<off_t>(offset), offset < 0 ? SEEK_END : SEEK_SET);
#endif
    if (result != 0) return Fail(errno);
    direction_ = Direction::None;
    return 0;
}

int64_t LocalFileIO::Tell() {
    if (!file_) return Fail(EBADF);
    errno = 0;
#if defined(__MINGW32__) && !defined(__MINGW64_VERSION_MAJOR)
    const auto offset = ::ftello64(file_);
#elif defined(_WIN32)
    const auto offset = _ftelli64(file_);
#else
    const auto offset = ::ftello(file_);
#endif
    return offset < 0 ? Fail(errno) : static_cast<int64_t>(offset);
}

int64_t LocalFileIO::Size() {
    if (!file_) return Fail(EBADF);
    const auto position = Tell();
    if (position < 0) return position;
    errno = 0;
#if defined(__MINGW32__) && !defined(__MINGW64_VERSION_MAJOR)
    if (::fseeko64(file_, 0, SEEK_END) != 0) return Fail(errno);
    const auto size = ::ftello64(file_);
#elif defined(_WIN32)
    if (_fseeki64(file_, 0, SEEK_END) != 0) return Fail(errno);
    const auto size = _ftelli64(file_);
#else
    if (::fseeko(file_, 0, SEEK_END) != 0) return Fail(errno);
    const auto size = ::ftello(file_);
#endif
    if (size < 0) return Fail(errno);
    const auto restore = Seek(position);
    return restore < 0 ? restore : static_cast<int64_t>(size);
}

int LocalFileIO::Flush() {
    if (!file_) return Fail(EBADF);
    // fflush on an input stream is not portable.
    if (direction_ != Direction::Write) return 0;
    errno = 0;
    return std::fflush(file_) == 0 ? 0 : Fail(errno);
}

int LocalFileIO::Close() {
    if (!file_) return 0;
    auto* file = file_;
    file_ = nullptr;
    writable_ = false;
    direction_ = Direction::None;
    errno = 0;
    return std::fclose(file) == 0 ? 0 : Fail(errno);
}

}
