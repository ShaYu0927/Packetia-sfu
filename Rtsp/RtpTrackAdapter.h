#ifndef PACKETIA_RTP_TRACK_ADAPTER_H_
#define PACKETIA_RTP_TRACK_ADAPTER_H_

#include "RtpTypes.h"
#include "RtpTrackBinding.h"

namespace sdp { struct SdpMedia; }

namespace media
{
// 旧接收链路的兼容边界；core 和公共 RTP 参数不包含 RtpTypes.h。
bool ToLegacyRtpTrackInfo(const RtpTrackContext& track, std::size_t selected,
                          ::TrackInfo& result, std::string* error = nullptr);
bool BuildRtpTrackInfo(const sdp::SdpMedia& media, int index, ::TrackInfo& result,
                       std::string* error = nullptr);
}

#endif
