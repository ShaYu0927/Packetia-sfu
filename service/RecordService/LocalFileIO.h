#pragma once

#include "ISeekableFile.h"
#include <cstdio>

namespace service {

// stdio-backed implementation. Opening and path policy are concrete backend
// concerns and intentionally do not belong to ISeekableFile.
class LocalFileIO final : public ISeekableFile {
public:
    enum class OpenMode { Create, ReadOnly };

    LocalFileIO() = default;
    ~LocalFileIO() override;
    LocalFileIO(const LocalFileIO&) = delete;
    LocalFileIO& operator=(const LocalFileIO&) = delete;

    // Create uses wb+ and truncates an existing file. The parent directory
    // must already exist. Opening an active instance returns -EBUSY.
    int Open(const std::string& path, OpenMode mode = OpenMode::Create);

    int Read(void* data, uint64_t bytes) override;
    int Write(const void* data, uint64_t bytes) override;
    int Seek(int64_t offset) override;
    int64_t Tell() override;
    int64_t Size() override;
    int Flush() override;
    int Close() override;
    bool IsOpen() const override { return file_ != nullptr; }
    std::string Error() const override;

    int ErrorCode() const { return error_code_; }

private:
    enum class Direction { None, Read, Write };
    int Fail(int code);
    int Prepare(Direction direction);

    std::FILE* file_ = nullptr;
    bool writable_ = false;
    Direction direction_ = Direction::None;
    int error_code_ = 0;
};

}
