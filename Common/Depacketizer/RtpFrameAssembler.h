#ifndef _RTP_FRAME_ASSEMBLER_H_
#define _RTP_FRAME_ASSEMBLER_H_

#include "H264Payload.h"

#include <deque>

namespace media
{

class RtpFrameAssembler
{
public:
    RtpFrameAssembler() = default;
    ~RtpFrameAssembler() = default;

    bool Input(const H264ParsedPacket& packet);

    bool HasFrame() const;
    bool PopFrame(H264AccessUnit& out);

    void Reset();

private:
    bool StartNewFrame(const H264ParsedPacket& packet);
    bool AppendPacket(const H264ParsedPacket& packet);
    bool FinishCurrentFrame();

    bool IsSeqContinuous(uint16_t prev, uint16_t cur) const
    {
        return static_cast<uint16_t>(prev + 1) == cur;
    }

private:
    bool has_current_ = false;
    bool broken_ = false;
    bool has_last_seq_ = false;

    uint16_t last_seq_ = 0;

    H264AccessUnit current_;
    std::deque<H264AccessUnit> ready_frames_;
};

} // namespace media

#endif // _RTP_FRAME_ASSEMBLER_H_