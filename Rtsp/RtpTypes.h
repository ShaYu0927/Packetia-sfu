#pragma once


#define RTP_HEADER_SIZE   	   12
#define MAX_RTP_PAYLOAD_SIZE   1420
#define RTP_MAX_PACKET_SIZE    (RTP_HEADER_SIZE + MAX_RTP_PAYLOAD_SIZE)
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


enum class MediaTransportType {
    TCP,
    UDP,
    UNKNOWN
};


// 每个 track 的信息
typedef struct RtpTrackInfo {
    int payload_type;       // RTP payload type，比如 96, 97
    std::string codec;      // 编码类型，比如 H265, MPEG4-GENERIC
    int clock_rate;         // 时钟频率，比如 90000, 44100
    int channels;           // 音频通道数，视频为 0 或 1

    std::string control;    // control:streamid=0
    std::string fmtp;       // 原始的 fmtp 整串
    std::string rtpmap;     // 原始的 rtpmap 整串

    // H.265 专用
    std::string vps;
    std::string sps;
    std::string pps;

    // AAC 专用
    std::string audio_config;  // config=1210
}RtpTrackInfo;

// 整个 SDP Session 信息
typedef struct RtspSessionDesc {
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
};

namespace rtp_limits 
{

    // max Rtp size
    static constexpr std::size_t kMaxPayloadBytes = 1200;

    // RTP fixed header = 12 bytes (不含 CSRC/extension)
    static constexpr std::size_t kMinHeaderBytes  = 12;

    // 允许的最大 RTP 包长度（header + payload）
    static constexpr std::size_t kMaxPacketBytes  = kMinHeaderBytes + kMaxPayloadBytes;

}
