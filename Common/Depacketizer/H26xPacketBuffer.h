#ifndef _H26X_PACKET_BUFFER_H_
#define _H26X_PACKET_BUFFER_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>
#include "H264Payload.h"

namespace media
{

class RtpSequenceNumberUnwrapper
{
public:
    void Reset();
    int64_t Unwrap(uint16_t sequence_number);
private:
    bool initialized_ = false;
    int64_t latest_unwrapped_seq_ = 0;
};

class H264PacketBuffer
{
public:
    struct Config
    {
        std::size_t max_frame_packets = 512;
        bool idr_only_keyframes_allowed = true;
    };
    struct InsertResult
    {
        bool inserted = false;
        bool duplicate = false;
        bool late = false;
        bool buffer_collision = false;
        std::vector<H264AccessUnit> frames;
    };

    H264PacketBuffer();
    explicit H264PacketBuffer(const Config& config);
    void Reset();
    InsertResult InsertPacket(H264ParsedPacket packet);
    size_t BufferedPacketCount() const noexcept { return buffered_packet_count_; }

private:
    struct BufferedPacket
    {
        int64_t unwrapped_seq = -1;
        H264ParsedPacket packet;
    };
    static constexpr size_t kBufferSize = 2048;
    static_assert((kBufferSize & (kBufferSize - 1)) == 0, "buffer size must be a power of two");

    static size_t BufferIndex(int64_t unwrapped_seq);
    BufferedPacket* GetPacket(int64_t unwrapped_seq);
    const BufferedPacket* GetPacket(int64_t unwrapped_seq) const;
    bool BeginningOfStream(const H264ParsedPacket& packet) const;
    bool CanAdvanceFrom(int64_t inserted_seq, const H264ParsedPacket& packet);
    int64_t FindFrameStart(int64_t end_seq) const;
    void FindFrames(int64_t inserted_seq, InsertResult& result);
    bool MaybeAssembleFrame(int64_t start_seq, int64_t end_seq, H264AccessUnit& frame);
    bool ValidatePacketUnits(const H264ParsedPacket& packet,bool& fu_active) const;
    bool AppendPacketUnits(H264ParsedPacket& packet, bool& fu_active, std::vector<uint8_t>& fu_nalu, H264AccessUnit& frame);
    void RemovePackets(int64_t start_seq, int64_t end_seq);
    void ClearInternal();

    Config config_;
    std::array<std::unique_ptr<BufferedPacket>, kBufferSize> buffer_{};
    RtpSequenceNumberUnwrapper seq_unwrapper_;
    std::optional<int64_t> last_continuous_unwrapped_seq_;
    size_t buffered_packet_count_ = 0;
};
} // namespace media

#endif // _H26X_PACKET_BUFFER_H_
