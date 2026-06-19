#ifndef _CORE_TYPE_H_
#define _CORE_TYPE_H_

namespace media
{

enum class ProtocolType 
{
    kUnknown = 0,

    kRtp,
    kRtsp,
    kRtmp,
    kHls,
    kWebRtc,
};

enum class coreMediaKind : int
{
    kUnknown = 0,

    kAudio,
    kVideo,
    kData,
};

enum class CodecType 
{
    kUnknown = 0,

    // Video
    kH264,
    kH265,
    kVp8,
    kVp9,
    kAv1,

    // Audio
    kOpus,
    kAac,
    kG711A,
    kG711U,
    kPcm,
};

enum class StreamDirection 
{
    kUnknown = 0,

    kInput,
    kOutput,

    kSend,
    kRecv,

    kPublish,
    kPlay,
};

inline const char* ProtocolTypeName(ProtocolType type) 
{
    switch (type) 
    {
        case ProtocolType::kRtp: return "RTP";
        case ProtocolType::kRtsp: return "RTSP";
        case ProtocolType::kRtmp: return "RTMP";
        case ProtocolType::kHls: return "HLS";
        case ProtocolType::kWebRtc: return "WebRTC";
        default: return "Unknown";
    }
}

inline const char* MediaKindName(coreMediaKind kind) 
{
    switch (kind) 
    {
        case coreMediaKind::kAudio: return "Audio";
        case coreMediaKind::kVideo: return "Video";
        case coreMediaKind::kData: return "Data";
        default: return "Unknown";
    }
}

inline const char* CodecTypeName(CodecType codec) 
{
    switch (codec) 
    {
        case CodecType::kH264: return "H264";
        case CodecType::kH265: return "H265";
        case CodecType::kVp8: return "VP8";
        case CodecType::kVp9: return "VP9";
        case CodecType::kAv1: return "AV1";
        case CodecType::kOpus: return "OPUS";
        case CodecType::kAac: return "AAC";
        case CodecType::kG711A: return "G711A";
        case CodecType::kG711U: return "G711U";
        case CodecType::kPcm: return "PCM";
        default: return "Unknown";
    }
}

inline const char* StreamDirectionName(StreamDirection direction) 
{
    switch (direction) 
    {
        case StreamDirection::kInput: return "Input";
        case StreamDirection::kOutput: return "Output";
        case StreamDirection::kSend: return "Send";
        case StreamDirection::kRecv: return "Recv";
        case StreamDirection::kPublish: return "Publish";
        case StreamDirection::kPlay: return "Play";
        default: return "Unknown";
    }
}

}

#endif