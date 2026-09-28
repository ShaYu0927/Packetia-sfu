#include "Mp4Reader.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <limits>

namespace service
{
bool Mp4Reader::Fail(const std::string& operation, int error)
{
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(error, message, sizeof(message));

    error_ = operation + ": " + message;
    return false;
}

bool Mp4Reader::Open(std::unique_ptr<ISeekableFile> file)
{
    Close();
    error_.clear();
    file_ = std::move(file);
    auto fail = [&](const char* operation, int code) {
        Fail(operation, code);
        Close();
        return false;
    };
    if (!file_ || !file_->IsOpen())
        return fail("open MP4 file", AVERROR(EBADF));

    int result = file_->Seek(0);
    if (result < 0)
        return fail("seek MP4 start", result);

    context_ = avformat_alloc_context();
    if (!context_)
        return fail("allocate format context", AVERROR(ENOMEM));

    constexpr int buffer_size = 64 * 1024;
    auto* buffer = static_cast<uint8_t*>(av_malloc(buffer_size));
    if (!buffer)
        return fail("allocate IO buffer", AVERROR(ENOMEM));

    io_context_ = avio_alloc_context(buffer, buffer_size, 0, file_.get(),
        &Mp4Reader::ReadPacket, nullptr, &Mp4Reader::Seek);
    if (!io_context_) {
        av_free(buffer);
        return fail("allocate IO context", AVERROR(ENOMEM));
    }
    io_context_->seekable = AVIO_SEEKABLE_NORMAL;
    context_->pb = io_context_;
    context_->flags |= AVFMT_FLAG_CUSTOM_IO;

    const auto* format = av_find_input_format("mov");
    if (!format)
        return fail("find MP4 demuxer", AVERROR_DEMUXER_NOT_FOUND);
    result = avformat_open_input(&context_, nullptr, format, nullptr);
    if (result < 0)
        return fail("open MP4 container", result);

    result = avformat_find_stream_info(context_, nullptr);
    if (result < 0)
        return fail("read MP4 tracks", result);
    if (!context_->nb_streams || context_->nb_streams > static_cast<unsigned>(INT_MAX))
        return fail("invalid MP4 track count", AVERROR_INVALIDDATA);
    // A demuxer may recover metadata despite a failed underlying read.
    if (io_context_->error < 0 && io_context_->error != AVERROR_EOF)
        return fail("read MP4 file", io_context_->error);
    return true;
}

ReadResult Mp4Reader::Read(AVPacket& packet)
{
    av_packet_unref(&packet);
    if (!error_.empty())
        return ReadResult::Error;
    if (!context_) {
        Fail("reader is closed", AVERROR(EBADF));
        return ReadResult::Error;
    }
    if (input_eof_)
        return ReadResult::End;

    const int result = av_read_frame(context_, &packet);
    if (result >= 0)
        return ReadResult::Frame;

    av_packet_unref(&packet);
    const int io_error = io_context_->error;
    if (io_error < 0 && io_error != AVERROR_EOF) {
        Fail("read MP4 file", io_error);
        return ReadResult::Error;
    }
    if (result == AVERROR_EOF) {
        input_eof_ = true;
        return ReadResult::End;
    }
    Fail("read MP4 packet", result);
    return ReadResult::Error;
}

int Mp4Reader::TrackCount() const
{
    return context_ ? static_cast<int>(context_->nb_streams) : 0;
}

const AVStream* Mp4Reader::Track(int index) const
{
    return index >= 0 && index < TrackCount() ? context_->streams[index] : nullptr;
}

int Mp4Reader::ReadPacket(void* opaque, uint8_t* data, int bytes)
{
    if(!opaque || !data || bytes <= 0)
        return AVERROR(EINVAL);

    auto* file = static_cast<ISeekableFile*>(opaque);
    const int64_t position = file->Tell();
    if(position < 0)
    {
        return static_cast<int>(position);
    }

    const int64_t size = file->Size();
    if (size < 0)
        return static_cast<int>(size);

    if (position >= size)
        return AVERROR_EOF;

    const int count = static_cast<int>(std::min<int64_t>(bytes, size - position));
    const int result = file->Read(data, count);

    return result < 0 ? result : count;
}


int64_t Mp4Reader::Seek(void* opaque, int64_t offset, int whence)
{
    if(!opaque)
    {
        return AVERROR(EINVAL);
    }

    auto* file = static_cast<ISeekableFile*>(opaque);

    if (whence & AVSEEK_SIZE)
        return file->Size();

    whence &= ~AVSEEK_FORCE;
    int64_t base = 0;

    switch(whence)
    {
    case SEEK_SET:
        break;
    case SEEK_CUR:
        base = file->Tell();
        break;
    case SEEK_END:
        base = file->Size();
        break;
    default:
        return AVERROR(EINVAL);
    }

    if(base < 0)
        return base;

    if (offset > 0 && base > std::numeric_limits<int64_t>::max() - offset)
        return AVERROR(EINVAL);

    if (offset < -base)
        return AVERROR(EINVAL);

    const int64_t target = base + offset;
    const int result = file->Seek(target);

    return result < 0 ? result : file->Tell();
}

Mp4Reader::~Mp4Reader()
{
    Close();
}

void Mp4Reader::Close()
{
    avformat_close_input(&context_);

    if (io_context_) 
    {
        av_freep(&io_context_->buffer);
        avio_context_free(&io_context_);
    }

    if (file_) {
        file_->Close();
        file_.reset();
    }

    input_eof_ = false;
}
}
