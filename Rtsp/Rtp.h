#ifndef _RTP_H_
#define _RTP_H_

#include <memory>
#include <string>
#include <variant>
#include <vector>

#define RTP_HEADER_SIZE   	   12
#define MAX_RTP_PAYLOAD_SIZE   1420
#define RTP_MAX_PACKET_SIZE    (RTP_HEADER_SIZE + MAX_RTP_PAYLOAD_SIZE)
#define RTP_VERSION			   2
#define RTP_TCP_HEAD_SIZE	   4
#define RTP_VPX_HEAD_SIZE	   1

#define MAX_MEDIA_CHANNEL 16

enum RTPTransportMode
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
};

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
struct RtspSessionDesc {
    std::string version;    // v=0
    std::string origin;     // o=...
    std::string session_name; // s=...
    std::string connection; // c=...
    std::string timing;     // t=...

    std::string tool;       // a=tool:libavformat...

    std::vector<RtpTrackInfo> tracks; // 多个 track
};


class RtpHeader {

public:
      static constexpr size_t kSize = 12;

    RtpHeader();

    // 序列化 RTP 头部到 buffer（12字节）
    void serialize(uint8_t* out) const;

    // Getter & Setter（主机字节序）
    uint8_t getVersion() const;
    void setVersion(uint8_t ver);

    uint8_t getPayloadType() const;
    void setPayloadType(uint8_t pt);

    bool getMarker() const;
    void setMarker(bool marker);

    uint16_t getSequence() const;
    void setSequence(uint16_t seq);

    uint32_t getTimestamp() const;
    void setTimestamp(uint32_t ts);

    uint32_t getSSRC() const;
    void setSSRC(uint32_t ssrc);

	uint16_t pt;  // 负载类型

private:
    uint8_t _version = 2;
    bool _padding = false;
    bool _extension = false;
    uint8_t _csrc = 0;

    bool _marker = false;
    uint8_t _payload_type = 0;

    uint16_t _seq = 0;
    uint32_t _timestamp = 0;
    uint32_t _ssrc = 0;
};



struct RtpTransportTcp {
    uint16_t rtp_channel = 0;
    uint16_t rtcp_channel = 0;
};

struct RtpTransportUdp {
    uint16_t rtp_port = 0;
    uint16_t rtcp_port = 0;
};

struct RtpTransportInfo {
    MediaTransportType transport_type = MediaTransportType::UNKNOWN;

    struct {
        uint16_t rtp_channel = 0;
        uint16_t rtcp_channel = 0;
    } tcp;

    struct {
        uint16_t rtp_port = 0;
        uint16_t rtcp_port = 0;
    } udp;
};


struct RtcpStats {
    uint64_t packet_count = 0;
    uint64_t octet_count = 0;
    uint64_t last_ntp_time = 0;
};

enum class MediaSessionState : uint8_t {
    Init   = 0,
    Setup  = 1,
    Play   = 2,
    Record = 4,
};

inline MediaSessionState operator|(MediaSessionState a, MediaSessionState b) {
    return static_cast<MediaSessionState>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}
inline bool hasState(MediaSessionState state, MediaSessionState flag) {
    return (static_cast<uint8_t>(state) & static_cast<uint8_t>(flag)) != 0;
}

struct MediaChannelInfo {
    RtpHeader rtp_header;

    RtpTransportInfo transport;

    uint16_t rtp_sequence = 0;
    uint32_t clock_rate = 90000;  // default for video

    RtcpStats rtcp_stats;

    MediaSessionState state = MediaSessionState::Init;

    void markSetup()  { state = state | MediaSessionState::Setup; }
    void markPlay()   { state = state | MediaSessionState::Play; }
    void markRecord() { state = state | MediaSessionState::Record; }

    bool isSetup() const  { return hasState(state, MediaSessionState::Setup); }
    bool isPlay() const   { return hasState(state, MediaSessionState::Play); }
    bool isRecord() const { return hasState(state, MediaSessionState::Record); }
};


class RtpPacket {
public:
    using Ptr = std::shared_ptr<RtpPacket>;

    static constexpr int kRtpVersion = 2;
    static constexpr int kRtpHeaderSize = 12;
    static constexpr int kRtpTcpHeaderSize = 4;
    static constexpr int kRtpMaxSize = 1500;  // 可以根据 MTU 设定

    // 创建一个 RTP 包并分配内存
    static Ptr create(size_t capacity = kRtpMaxSize);

    // RTP header 解析相关
    uint16_t getSeq() const;
    void setSeq(uint16_t seq);

    uint32_t getStamp() const;
    void setStamp(uint32_t ts);

    uint64_t getStampMS(bool ntp = true) const;

    uint32_t getSSRC() const;
    void setSSRC(uint32_t ssrc);

    uint8_t* getPayload();                 // 指向负载起始
    size_t getPayloadSize() const;        // 返回负载长度

    // 设置负载数据
    void setPayload(const uint8_t* payload_data, size_t len);

    // 设置 marker bit
    void setMarker(bool val);
    bool getMarker() const;

    void setPayloadType(uint8_t pt);
    uint8_t getPayloadType() const;

    void setVersion(uint8_t version);
    uint8_t getVersion() const;

    // 原始 RTP 包数据
    std::shared_ptr<uint8_t> data;
    size_t size = 0;       // 实际使用的长度

    // 业务层拓展字段
    TrackType type = TrackInvalid;
    uint32_t sample_rate = 90000;
    uint64_t ntp_stamp_ms = 0;
    int track_index = -1;

    RtpPacket() = default;
private:
    

    // 用于创建 shared_ptr<RtpPacket>
    static Ptr alloc(size_t capacity);
};


#endif