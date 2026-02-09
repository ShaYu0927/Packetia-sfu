#include "H264Depacketizer.h"


bool H264Depacketizer::input(const Ptr& pkt)
{
    return true;
}

bool H264Depacketizer::hasFrame() const
{
    return false;
}

std::vector<uint8_t> H264Depacketizer::popFrame()
{
    return {};
}