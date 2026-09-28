#ifndef _MP4_READER_H_
#define _MP4_READER_H_

#include "ISeekableFile.h"
#include "IMediaReader.h"

#include <memory>
#include <string>

struct AVFormatContext;
struct AVIOContext;


namespace service
{

class Mp4Reader final : public IMediaReader
{
public:
    Mp4Reader() = default;
    ~Mp4Reader() override;

    Mp4Reader(const Mp4Reader&) = delete;
    Mp4Reader& operator=(const Mp4Reader&) = delete;

    /* 接管已打开且内容不再变化的只读切片，从文件起点读取。
       无论成功还是失败，均由 reader 负责关闭。 */
    bool Open(std::unique_ptr<ISeekableFile> file);
    
    ReadResult Read(AVPacket& packet) override;
    int TrackCount() const override;
    const AVStream* Track(int index) const override;

    void Close() override;

    const std::string& Error() const override { return error_; }

private:
    static int ReadPacket(void* opaque, uint8_t* data, int bytes);
    static int64_t Seek(void* opaque, int64_t offset, int whence);

    bool Fail(const std::string& operation, int error);

    std::unique_ptr<ISeekableFile> file_;

    AVFormatContext* context_ = nullptr;
    AVIOContext* io_context_ = nullptr;
    bool input_eof_ = false;

    std::string error_;
};

}

#endif /* _MP4_READER_H_ */
