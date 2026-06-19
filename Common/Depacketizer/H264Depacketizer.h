#ifndef _H264DEPACKETIZER_H_
#define _H264DEPACKETIZER_H_

#include "Depacketizer.h"

#include <vector>
#include <cstdint>
#include "H264RtpPayloadParser.h"
#include "RtpFrameAssembler.h"


class H264Depacketizer : public Depacketizer
{
public:
    bool input(const RtpView& pkt) override;
    bool hasFrame() const override;
    std::vector<uint8_t> popFrame() override;

private:
    media::H264RtpPayloadParser parser_;
    media::RtpFrameAssembler assembler_;
};

#endif