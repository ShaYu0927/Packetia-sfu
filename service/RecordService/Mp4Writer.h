#pragma once

#include "ISeekableFile.h"
#include "core/EncodedFrameRouter.h"
#include <map>
#include <memory>
#include <string>
#include <vector>

struct AVFormatContext;
struct AVPacket;
struct AVIOContext;

namespace service {

// Container muxing only. The writer owns an already-open random-access file;
// all methods and file callbacks stay on the assigned recording worker.
class Mp4Writer 
{
public:
    ~Mp4Writer();
    // Takes ownership on success or failure and closes the file in Close().
    bool Open(std::unique_ptr<ISeekableFile> file,
              const std::vector<media::EncodedFrameEvent>& tracks);
    bool Write(const media::EncodedFrameEvent& event, int64_t timestamp_us);
    bool Close();
    const std::string& Error() const { return error_; }
    static bool Ready(const media::EncodedFrame& frame);
private:
    bool Fail(const std::string& operation, int error);
    bool FailFile(const std::string& operation);
    static int WritePacket(void* opaque, uint8_t* data, int bytes);
    static int64_t Seek(void* opaque, int64_t offset, int whence);
    struct Track {
        int index = 0;
        AVPacket* pending = nullptr;
        int64_t last_duration = 0;
    };
    AVFormatContext* context_ = nullptr;
    AVIOContext* io_context_ = nullptr;
    std::unique_ptr<ISeekableFile> file_;
    bool header_written_ = false;
    std::map<uint64_t, Track> tracks_;
    std::string error_;
};
}
