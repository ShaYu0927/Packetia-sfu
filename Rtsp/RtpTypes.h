#pragma once


#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <array>
#include <unordered_map>
#include "Rtsp.h"


static constexpr size_t kRtpHeaderSize = 12;
static constexpr size_t kMaxRtpPayloadSize = 1420;
static constexpr size_t kRtpMaxPacketSize = kRtpHeaderSize + kMaxRtpPayloadSize;
#define RTP_VERSION			   2
#define RTP_TCP_HEAD_SIZE	   4
#define RTP_VPX_HEAD_SIZE	   1

#define MAX_MEDIA_CHANNEL 16


typedef enum RTPTransportMode
{
    RTP_OVER_TCP = 0,
    RTP_OVER_UDP,
    RTP_OVER_MULTICAST,
    RTP_OVER_HTTP,
    RTP_OVER_FILE,
    RTP_OVER_UNIX_DOMAIN_SOCKET,
    RTP_OVER_SSL,
    RTP_OVER_TLS,
    RTP_OVER_WEBSOCKET,
    RTP_OVER_QUIC,
	RTP_OVER_UNKNOWN
}RTPTransportMode;

enum TrackType
{	
	TrackInvalid = -1,
    TrackVideo = 0,
    TrackAudio,
    TrackTitle,
    TrackApplication,
    TrackMax
};

enum class RTPVERSION
{
    VERSION_ONE
};

enum class MediaTransportType 
{
    TCP,
    UDP,
    UNKNOWN
};


/* track Info */
typedef struct RtpTrackInfo 
{
    int payload_type;       
    std::string codec;      
    int clock_rate;         
    int channels;           

    std::string control;    
    std::string fmtp;       
    std::string rtpmap;    

    std::string vps;
    std::string sps;
    std::string pps;

    std::string audio_config;  
}RtpTrackInfo;

typedef struct RtspSessionDesc 
{
    std::string version;    // v=0
    std::string origin;     // o=...
    std::string session_name; // s=...
    std::string connection; // c=...
    std::string timing;     // t=...

    std::string tool;       // a=tool:libavformat...

    std::vector<RtpTrackInfo> tracks; // 多个 track
}RtspSessionDesc;


struct TcpChannel 
{
    uint8_t rtp;
    uint8_t rtcp;
};

struct RtpRawPacket 
{
    using Ptr = std::shared_ptr<RtpRawPacket>;

    std::shared_ptr<uint8_t> data; 
    size_t size = 0;


    TrackType track = TrackInvalid;
    uint32_t clock_rate = 90000;


    uint32_t ssrc = 0;
    uint16_t seq = 0;
    uint32_t timestamp = 0;
    uint8_t payload_type = 0;
    bool marker = false;

    std::size_t header_size = kRtpHeaderSize;
    std::size_t payload_offset = kRtpHeaderSize;
    std::size_t payload_size = 0;
};

struct RtpHeaderFields 
{
    RTPVERSION version = RTPVERSION::VERSION_ONE;
    bool padding = false;
    bool extension = false;
    uint8_t cc = 0;
    bool marker = false;
    uint8_t payload_type = 0;
    uint16_t seq = 0;
    uint32_t timestamp = 0;
    uint32_t ssrc = 0;
    uint8_t csrc_count = 0;
    std::array<uint32_t, 15> csrc{};
};

struct PacketBufferView
{
    std::shared_ptr<uint8_t[]> data;
    size_t capacity = 0;
    size_t size = 0;
    uint16_t header_len = kRtpHeaderSize;
    uint16_t payload_offset = kRtpHeaderSize;
    uint32_t payload_len = 0;
};

struct RtpPacketMeta 
{
    TrackType track_type = TrackInvalid;
    uint32_t sample_rate = 90000;
    uint64_t ntp_stamp_ms = 0;
    int track_index = -1;
    uint32_t recv_time_ms = 0;
};

enum class CodecId
{
    Unknown = 0,
    H264,
    H265,

    // audio
    PCMU,
    PCMA,
    OPUS,
    AAC
};

struct CodecTraits
{
    CodecId id;
    uint32_t default_clock_rate;
    bool is_audio;
};

inline const CodecTraits* GetCodecTraits(CodecId id)
{
    static const std::unordered_map<CodecId, CodecTraits> kTraits = {
        {CodecId::OPUS, {CodecId::OPUS, 48000, true}},
        {CodecId::PCMU, {CodecId::PCMU, 8000, true}},
        {CodecId::H264, {CodecId::H264, 90000, false}},
        {CodecId::H265, {CodecId::H265, 90000, false}},
    };

    auto it = kTraits.find(id);
    if (it != kTraits.end())
        return &it->second;

    return nullptr;
}


struct TrackInfo
{
    RtspTransport rtsp_transport;

    TrackType type = TrackInvalid;
    CodecId codec_id = CodecId::Unknown;
    std::string codec_name;

    int track_index = -1;

    uint32_t ssrc = 0;
    uint32_t clock_rate = 0;
    uint8_t payload_type = 0xFF;
    // SDP 协商得到的 transport-wide-cc RTP Header Extension ID，0 表示禁用。
    uint8_t transport_cc_extension_id = 0;
    int channels = 0;

    std::string control;
    std::string fmtp;
};

struct RtpTrackStats
{
    uint64_t received_packets = 0;
    uint64_t sorted_packets = 0;
    uint64_t dropped_packets = 0;
    uint64_t lost_packets = 0;
    uint64_t duplicate_packets = 0;
    uint64_t out_of_order_packets = 0;
    uint64_t pool_exhausted_packets = 0;
    uint64_t oversized_packets = 0;

    uint16_t last_seq = 0;
    uint32_t last_ts = 0;
    bool seen_packet = false;
};


inline void NormalizeTrackInfo(TrackInfo& info)
{
    if (info.clock_rate == 0)
    {
        if (const auto* traits = GetCodecTraits(info.codec_id))
        {
            info.clock_rate = traits->default_clock_rate;
        }
    }

    if (info.type == TrackInvalid)
    {
        if (const auto* traits = GetCodecTraits(info.codec_id))
        {
            info.type = traits->is_audio ? TrackAudio : TrackVideo;
        }
    }
}

inline CodecId StringToCodecId(const std::string& name)
{
    std::string codec = name;
    std::transform(codec.begin(), codec.end(), codec.begin(),
                   [](unsigned char c) { return std::toupper(c); });

    if (codec == "H264") return CodecId::H264;
    if (codec == "H265" || codec == "HEVC") return CodecId::H265;
    if (codec == "PCMU") return CodecId::PCMU;
    if (codec == "PCMA") return CodecId::PCMA;
    if (codec == "OPUS") return CodecId::OPUS;
    if (codec == "AAC" || codec == "MPEG4-GENERIC") return CodecId::AAC;

    return CodecId::Unknown;
}

inline const char* TrackTypeToString(TrackType type)
{
    switch (type)
    {
    case TrackType::TrackAudio:
        return "audio";
    case TrackType::TrackVideo:
        return "video";
    default:
        return "invalid";
    }
}
