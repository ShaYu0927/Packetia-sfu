#ifndef PACKETIA_MEDIA_CODEC_NAMES_H_
#define PACKETIA_MEDIA_CODEC_NAMES_H_

#include "MediaTypes.h"
#include <string_view>

namespace media
{
// 只识别格式名称和别名，不表示当前接入链路支持该编码。
inline CodecType CodecTypeFromName(std::string_view name)
{
    const auto matches = [name](std::string_view expected) {
        if (name.size() != expected.size()) return false;
        for (std::size_t i = 0; i < name.size(); ++i)
        {
            char c = name[i];
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
            if (c != expected[i]) return false;
        }
        return true;
    };
    if (matches("h264")) return CodecType::H264;
    if (matches("h265") || matches("hevc")) return CodecType::H265;
    if (matches("opus")) return CodecType::Opus;
    if (matches("pcmu")) return CodecType::PCMU;
    if (matches("pcma")) return CodecType::PCMA;
    if (matches("aac") || matches("mpeg4-generic")) return CodecType::AAC;
    if (matches("vp8")) return CodecType::VP8;
    if (matches("vp9")) return CodecType::VP9;
    if (matches("av1")) return CodecType::AV1;
    return CodecType::Unknown;
}
}

#endif
