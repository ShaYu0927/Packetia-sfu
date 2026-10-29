#ifndef PACKETIA_RTP_TRACK_BINDING_H_
#define PACKETIA_RTP_TRACK_BINDING_H_

#include "MediaTrackDescription.h"
#include "RtpStreamContext.h"
#include <cstddef>
#include <string>

namespace media
{
bool SelectRtpPayload(const RtpTrackContext& track, std::size_t& selected, std::string* error = nullptr);
bool DescribeMediaTrack(const RtpTrackContext& track, std::size_t selected, MediaTrackDescription& result, std::string* error = nullptr);
}

#endif
