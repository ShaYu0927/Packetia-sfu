#include "Mp4Reader.h"
#include "LocalFileIO.h"
extern "C" {
#include <libavformat/avformat.h>
}
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error(#condition); } while (false)

namespace {
struct Packet {
    AVPacket* value = av_packet_alloc();
    Packet() { CHECK(value); }
    ~Packet() { av_packet_free(&value); }
};
struct Input {
    AVFormatContext* value = nullptr;
    ~Input() { avformat_close_input(&value); }
};
struct State {
    int closes = 0;
    int fault = 0;
};

// Exact-read backend: any request beyond EOF fails, like LocalFileIO.
// This exercises the AVIO adapter's final short buffer and ownership contract.
class MemoryFile final : public service::ISeekableFile {
public:
    MemoryFile(const std::vector<uint8_t>& data, std::shared_ptr<State> state)
        : data_(data), state_(std::move(state)) {}
    int Read(void* output, uint64_t bytes) override {
        if (state_->fault == 1 || position_ > data_.size() || bytes > data_.size() - position_)
            return -EIO;
        std::memcpy(output, data_.data() + position_, static_cast<size_t>(bytes));
        position_ += bytes;
        return 0;
    }
    int Write(const void*, uint64_t) override { return -EBADF; }
    int Seek(int64_t offset) override {
        if (state_->fault == 2) return -EIO;
        if (offset < 0) offset += static_cast<int64_t>(data_.size());
        if (offset < 0) return -EINVAL;
        position_ = static_cast<uint64_t>(offset);
        return 0;
    }
    int64_t Tell() override { return state_->fault == 3 ? -EIO : static_cast<int64_t>(position_); }
    int64_t Size() override { return state_->fault == 4 ? -EIO : static_cast<int64_t>(data_.size()); }
    int Flush() override { return 0; }
    int Close() override { ++state_->closes; open_ = false; return 0; }
    bool IsOpen() const override { return open_; }
    std::string Error() const override { return "injected file error"; }
private:
    const std::vector<uint8_t>& data_;
    std::shared_ptr<State> state_;
    uint64_t position_ = 0;
    bool open_ = true;
};

void Compare(service::IMediaReader& reader, const char* path) {
    Input reference;
    CHECK(avformat_open_input(&reference.value, path, nullptr, nullptr) == 0);
    CHECK(avformat_find_stream_info(reference.value, nullptr) >= 0);
    CHECK(reader.TrackCount() == static_cast<int>(reference.value->nb_streams));
    CHECK(!reader.Track(-1) && !reader.Track(reader.TrackCount()));
    for (int i = 0; i < reader.TrackCount(); ++i) {
        const auto* a = reader.Track(i);
        const auto* b = reference.value->streams[i];
        CHECK(a->codecpar->codec_id == b->codecpar->codec_id);
        CHECK(a->codecpar->codec_type == b->codecpar->codec_type);
        CHECK(a->time_base.num == b->time_base.num && a->time_base.den == b->time_base.den);
        CHECK(a->codecpar->extradata_size == b->codecpar->extradata_size);
        if (a->codecpar->extradata_size)
            CHECK(std::memcmp(a->codecpar->extradata, b->codecpar->extradata, a->codecpar->extradata_size) == 0);
    }
    Packet actual, expected, retained;
    int packets = 0;
    std::vector<uint8_t> first;
    for (;;) {
        const int result = av_read_frame(reference.value, expected.value);
        if (result == AVERROR_EOF) break;
        CHECK(result >= 0);
        CHECK(reader.Read(*actual.value) == service::ReadResult::Frame);
        const auto& a = *actual.value;
        const auto& b = *expected.value;
        CHECK(a.stream_index == b.stream_index && a.pts == b.pts && a.dts == b.dts);
        CHECK(a.duration == b.duration && a.flags == b.flags && a.size == b.size);
        CHECK(std::memcmp(a.data, b.data, a.size) == 0);
        if (!packets++) {
            CHECK(av_packet_ref(retained.value, actual.value) == 0);
            first.assign(a.data, a.data + a.size);
        }
        av_packet_unref(expected.value);
        // Read replaces the previous packet reference without requiring caller unref.
    }
    CHECK(packets > 0);
    CHECK(reader.Read(*actual.value) == service::ReadResult::End);
    CHECK(!actual.value->data && !actual.value->buf);
    CHECK(reader.Read(*actual.value) == service::ReadResult::End);
    CHECK(reader.Error().empty());
    reader.Close();
    reader.Close();
    CHECK(reader.TrackCount() == 0 && !reader.Track(0));
    CHECK(std::vector<uint8_t>(retained.value->data, retained.value->data + retained.value->size) == first);
    CHECK(reader.Read(*actual.value) == service::ReadResult::Error);
    CHECK(!reader.Error().empty());
}
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    try {
        std::ifstream input(argv[1], std::ios::binary);
        CHECK(input);
        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
        CHECK(!bytes.empty());
        service::Mp4Reader reader;
        CHECK(!reader.Open(nullptr));
        CHECK(!reader.Error().empty());
        auto closed = std::make_unique<service::LocalFileIO>();
        CHECK(!reader.Open(std::move(closed)));

        auto local = std::make_unique<service::LocalFileIO>();
        CHECK(local->Open(argv[1], service::LocalFileIO::OpenMode::ReadOnly) == 0);
        CHECK(local->Seek(11) == 0); // Open must reset the supplied file position.
        CHECK(reader.Open(std::move(local)) && reader.Error().empty());
        Compare(reader, argv[1]);

        auto state = std::make_shared<State>();
        CHECK(reader.Open(std::make_unique<MemoryFile>(bytes, state)));
        Compare(reader, argv[1]);
        CHECK(state->closes == 1);

        auto previous = std::make_shared<State>();
        CHECK(reader.Open(std::make_unique<MemoryFile>(bytes, previous)));
        auto replacement = std::make_shared<State>();
        CHECK(reader.Open(std::make_unique<MemoryFile>(bytes, replacement)));
        CHECK(previous->closes == 1);
        reader.Close();
        CHECK(replacement->closes == 1);

        for (int fault = 1; fault <= 4; ++fault) {
            auto failed = std::make_shared<State>();
            failed->fault = fault;
            CHECK(!reader.Open(std::make_unique<MemoryFile>(bytes, failed)));
            CHECK(failed->closes == 1 && !reader.Error().empty());
            CHECK(reader.TrackCount() == 0);
            Packet packet;
            CHECK(reader.Read(*packet.value) == service::ReadResult::Error);
            reader.Close();
            CHECK(failed->closes == 1);
        }
        const std::vector<uint8_t> invalid(23, 0);
        auto bad = std::make_shared<State>();
        CHECK(!reader.Open(std::make_unique<MemoryFile>(invalid, bad)));
        CHECK(bad->closes == 1);
        auto final_state = std::make_shared<State>();
        {
            service::Mp4Reader scoped;
            CHECK(scoped.Open(std::make_unique<MemoryFile>(bytes, final_state)));
        }
        CHECK(final_state->closes == 1);
        std::cout << "Passed: packet/track identity, exact-read IO, EOF, reopen, ownership and failures\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
