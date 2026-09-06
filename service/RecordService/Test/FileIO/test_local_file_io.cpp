#include "ISeekableFile.h"
#include "LocalFileIO.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>

#define CHECK(expr) do { if (!(expr)) throw std::runtime_error(#expr); } while (false)

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::string path = argv[1];
    struct Cleanup {
        const std::string& path;
        ~Cleanup() { std::remove(path.c_str()); }
    } cleanup{path};
    try {
        using service::ISeekableFile;
        using service::LocalFileIO;
        static_assert(std::is_base_of<ISeekableFile, LocalFileIO>::value,
                      "LocalFileIO implements the common file contract");
        static_assert(!std::is_copy_constructible<LocalFileIO>::value, "Unique file ownership");
        LocalFileIO io;
        ISeekableFile& generic = io;
        char data[8]{};
        CHECK(io.Read(data, 1) == -EBADF);
        CHECK(io.Write("x", 1) == -EBADF);
        CHECK(io.Seek(0) == -EBADF);
        CHECK(io.Tell() == -EBADF);
        CHECK(io.Flush() == -EBADF);
        CHECK(io.Close() == 0);
        CHECK(io.Open("") == -EINVAL);
        CHECK(io.Open(std::string("bad\0path", 8)) == -EINVAL);
        CHECK(io.Open(path) == 0);
        CHECK(io.ErrorCode() == 0);
        CHECK(generic.Write("abcdefgh", 8) == 0);
        CHECK(io.Tell() == 8);
        CHECK(generic.Size() == 8);
        CHECK(generic.Tell() == 8); // Size preserves the current position
        CHECK(io.Open(path) == -EBUSY); // must not truncate the active file
        CHECK(io.Tell() == 8);
        CHECK(io.Seek(2) == 0);
        CHECK(io.Write("XY", 2) == 0); // patch an MP4 box/header in place
        CHECK(io.Read(data, 2) == 0); // write -> read without explicit seek
        CHECK(std::memcmp(data, "ef", 2) == 0);
        CHECK(io.Write("Z", 1) == 0); // read -> write without explicit seek
        CHECK(io.Flush() == 0);
        CHECK(io.Seek(-2) == 0);
        CHECK(io.Read(data, 2) == 0);
        CHECK(std::memcmp(data, "Zh", 2) == 0);
        CHECK(io.Read(data, 1) < 0); // EOF must not look like success
        CHECK(!io.Error().empty());
        CHECK(io.Seek(0) == 0); // recover from EOF
        CHECK(io.Read(data, 8) == 0);
        CHECK(std::memcmp(data, "abXYefZh", 8) == 0);
        CHECK(io.Read(nullptr, 0) == 0);
        CHECK(io.Write(nullptr, 0) == 0);
        CHECK(io.Read(nullptr, 1) == -EINVAL);
        CHECK(io.Write(nullptr, 1) == -EINVAL);
        // Exercise 64-bit positioning without allocating a huge file.
        const int64_t large_offset = (int64_t{1} << 32) + 123;
        CHECK(io.Seek(large_offset) == 0);
        CHECK(io.Tell() == large_offset);
        CHECK(io.Close() == 0);
        CHECK(!io.IsOpen());
        CHECK(io.Close() == 0);
        CHECK(io.Open(path, LocalFileIO::OpenMode::ReadOnly) == 0);
        CHECK(io.ErrorCode() == 0);
        CHECK(io.Read(data, 8) == 0);
        CHECK(std::memcmp(data, "abXYefZh", 8) == 0);
        CHECK(io.Write("x", 1) == -EBADF);
        CHECK(io.Close() == 0);
        CHECK(io.Open(path + "/missing/file") < 0);
        CHECK(!io.IsOpen());
        {
            LocalFileIO scoped;
            CHECK(scoped.Open(path) == 0); // explicit Create truncates
            CHECK(scoped.Write("ok", 2) == 0);
        } // destructor flushes/closes
        CHECK(io.Open(path, LocalFileIO::OpenMode::ReadOnly) == 0);
        CHECK(io.Read(data, 2) == 0);
        CHECK(std::memcmp(data, "ok", 2) == 0);
        CHECK(io.Read(data, 1) < 0);
        CHECK(io.Close() == 0);
#ifdef __linux__
        CHECK(io.Open("/dev/full") == 0);
        const int write_result = io.Write("x", 1);
        const int flush_result = io.Flush();
        CHECK(write_result < 0 || flush_result < 0);
        CHECK(io.ErrorCode() != 0);
        io.Close();
#endif
        std::cout << "Seekable local file IO checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MP4 file IO check failed: " << error.what() << '\n';
        return 1;
    }
}
