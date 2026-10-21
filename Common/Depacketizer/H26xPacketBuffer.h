#ifndef _H26X_PACKET_BUFFER_H_
#define _H26X_PACKET_BUFFER_H_

#include "PoolAllocator.h"
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <map>
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
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    struct Config
    {
        std::size_t max_frame_packets = 512;
        bool idr_only_keyframes_allowed = true;
        uint32_t max_wait_ms = 100;
    };
    struct InsertResult
    {
        bool inserted = false;
        bool duplicate = false;
        bool late = false;
        bool buffer_collision = false;
        size_t expired_packets = 0;
        std::vector<H264AccessUnit> frames;
    };

    H264PacketBuffer();
    explicit H264PacketBuffer(const Config& config);
    void Reset();
    InsertResult InsertPacket(H264ParsedPacket packet, TimePoint now = Clock::now());
    // An expired partial AU/reference chain is discarded, never emitted as a
    // complete frame. A fresh first IDR slice/SPS is required to resume.
    size_t FlushExpired(TimePoint now = Clock::now());
    size_t BufferedPacketCount() const noexcept { return buffered_packet_count_; }

private:
    using Arrivals = std::multimap<TimePoint, int64_t>;
    struct BufferedPacket
    {
        int64_t unwrapped_seq = -1;
        H264ParsedPacket packet;
        Arrivals::iterator arrival;
    };
    static constexpr size_t kBufferSize = 2048;
    static_assert((kBufferSize & (kBufferSize - 1)) == 0, "buffer size must be a power of two");

    static size_t BufferIndex(int64_t unwrapped_seq);
    BufferedPacket* GetPacket(int64_t unwrapped_seq);
    const BufferedPacket* GetPacket(int64_t unwrapped_seq) const;
    bool BeginningOfStream(const H264ParsedPacket& packet) const;
    bool CanAdvanceFrom(int64_t inserted_seq, const H264ParsedPacket& packet);
    void FindFrames(int64_t inserted_seq, InsertResult& result);
    bool MaybeAssembleFrame(int64_t start_seq, int64_t end_seq, H264AccessUnit& frame);
    bool ValidatePacketUnits(const H264ParsedPacket& packet,
                             std::optional<uint8_t>& fu_header) const;
    bool AppendPacketUnits(H264ParsedPacket& packet, bool& fu_active, common::ByteVector& fu_nalu, H264AccessUnit& frame);
    void RemovePackets(int64_t start_seq, int64_t end_seq);
    void RemovePacketsBefore(int64_t seq);
    void DropCurrentFrame(int64_t end_seq);
    void ClearInternal();
    void ErasePacket(std::unique_ptr<BufferedPacket>& packet);

    Config config_;
    std::array<std::unique_ptr<BufferedPacket>, kBufferSize> buffer_{};
    Arrivals arrivals_;
    RtpSequenceNumberUnwrapper seq_unwrapper_;
    std::optional<int64_t> last_continuous_unwrapped_seq_;
    std::optional<uint32_t> last_continuous_timestamp_;
    std::optional<int64_t> current_frame_start_;
    uint32_t current_frame_timestamp_ = 0;
    bool discarding_frame_ = false;
    bool waiting_for_recovery_ = false;
    size_t buffered_packet_count_ = 0;
};
} // namespace media

#endif // _H26X_PACKET_BUFFER_H_
