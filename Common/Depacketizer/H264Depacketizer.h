#ifndef _H264DEPACKETIZER_H_
#define _H264DEPACKETIZER_H_

#include "Depacketizer.h"

#include <vector>
#include <deque>
#include <cstdint>
#include <string>
#include "H264RtpPayloadParser.h"
#include "H26xPacketBuffer.h"
#include "H264ParameterSetTracker.h"


class H264Depacketizer : public Depacketizer
{
public:
    explicit H264Depacketizer(const std::string& fmtp = {});
    bool input(const RtpView& pkt) override;
    bool hasFrame() const override;
    std::vector<uint8_t> popFrame() override;
    bool popAccessUnit(media::H264AccessUnit& out);
    const media::H264ParameterSetTracker& parameterSets() const { return parameter_sets_; }

private:
    media::H264RtpPayloadParser parser_;
    media::H264PacketBuffer packet_buffer_;
    media::H264ParameterSetTracker parameter_sets_;
    std::deque<media::H264AccessUnit> ready_frames_;
};

#endif
