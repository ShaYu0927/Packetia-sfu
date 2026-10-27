#ifndef PACKETIA_MEDIA_RTP_TRACK_BINDING_H_
#define PACKETIA_MEDIA_RTP_TRACK_BINDING_H_

#include "MediaStreamTrack.h"
#include "RtpStreamContext.h"
#include <cstddef>
#include <string>

namespace media
{
bool SelectRtpPayload(const RtpTrackContext& track, std::size_t& selected, std::string* error = nullptr);
bool DescribeMediaTrack(const RtpTrackContext& track, std::size_t selected, MediaStreamTrack& result, std::string* error = nullptr);
}

#endif
