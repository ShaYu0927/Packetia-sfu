#ifndef _H26X_PACKET_BUFFER_H_
#define _H26X_PACKET_BUFFER_H_

#include <array>
#include <cstddef>
#include <cstdint>
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

class H26xPacketBuffer
{
public:
    struct Config
    {
        std::size_t max_frame_packets = 512;
        int64_t max_frame_delay_ms = 100;
        bool allow_ends_frame = true;
    };
    struct InsertResult
    {
        bool inserted = false;
        bool duplicate = false;
        bool late = false;
        bool buffer_collision = false;
        bool buffer_reset = false;
        std::size_t dropped_frames = 0;
        std::vector<H264AccessUnit> frames;
    };

    H26xPacketBuffer();
    explicit H26xPacketBuffer(const Config& config);
    void Reset();
    InsertResult InsertPacket(H264ParsedPacket packet, int64_t now_ms = -1);

private:
    struct BufferedPacket
    {
        int64_t unwrapped_seq = -1;
        int64_t arrival_ms = 0;
        bool occupied = false;
        H264ParsedPacket packet;
        void Reset()
        {
            occupied = false; unwrapped_seq = -1; arrival_ms = 0; packet.Reset();
        }
    };
    static constexpr size_t kBufferSize = 2048;
    static_assert((kBufferSize & (kBufferSize - 1)) == 0, "buffer size must be a power of two");

    size_t BufferedPacketCount() const;
    static int64_t GetNowMs();
    static size_t BufferIndex(int64_t unwrapped_seq);
    BufferedPacket* GetPacket(int64_t unwrapped_seq);
    const BufferedPacket* GetPacket(int64_t unwrapped_seq) const;
    bool IsPacketStale(int64_t unwrapped_seq) const;
    bool IsFrameEnd(const H264ParsedPacket& packet) const;
    bool BeginningOfStream(int64_t unwrapped_seq, const H264ParsedPacket& packet) const;
    void FindFrames(int64_t inserted_seq, InsertResult& result);
    void CollectCandidateFrameEnds(int64_t inserted_seq, std::vector<int64_t>& candidates) const;
    bool MaybeAssembleFrame(int64_t end_seq, H264AccessUnit& frame, int64_t& start_seq);
    bool FindFrameStart(int64_t end_seq, int64_t& start_seq) const;
    bool AssembleFrame(int64_t start_seq, int64_t end_seq, H264AccessUnit& frame) const;
    bool AppendPacketUnits(const H264ParsedPacket& packet, bool& fu_active, std::vector<uint8_t>& fu_nalu, H264AccessUnit& frame) const;
    void ClearPackets(int64_t start_seq, int64_t end_seq);
    void ClearFrameByTimestamp(uint32_t ssrc, uint32_t timestamp);
    void DropExpiredFrames(int64_t now_ms, InsertResult& result);
    void RecoverFromBufferCollision(int64_t new_unwrapped_seq);

    Config config_;
    bool has_received_packet_ = false;
    std::array<BufferedPacket, kBufferSize> buffer_{};
    RtpSequenceNumberUnwrapper seq_unwrapper_;
    bool has_last_output_seq_ = false;
    int64_t last_output_seq_ = -1;
    size_t buffered_packet_count_ = 0;
};
} // namespace media

#endif // _H26X_PACKET_BUFFER_H_
