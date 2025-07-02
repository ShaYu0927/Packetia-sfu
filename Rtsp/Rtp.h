#ifndef _RTP_H_
#define _RTP_H_

#include <memory>
#include <string>

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


class RtpHeader {
public:
    static constexpr size_t kSize = 12;

    RtpHeader();

    // 解析RTP头部（从裸数据）
    bool parse(const uint8_t* data, size_t size);

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



struct MediaChannelInfo
{
	RtpHeader rtp_header;

	// tcp
	uint16_t rtp_channel;
	uint16_t rtcp_channel;

	// udp
	uint16_t rtp_port;
	uint16_t rtcp_port;
	uint16_t packet_seq;
	uint32_t clock_rate;

	// rtcp
	uint64_t packet_count;
	uint64_t octet_count;
	uint64_t last_rtcp_ntp_time;

	bool is_setup;
	bool is_play;
	bool is_record;
};

//整个RTP包的结构体
class RtpPacket
{
public:
	using Ptr = std::shared_ptr<RtpPacket>;
	


	// 主机字节序的seq
    uint16_t getSeq() const;
    uint32_t getStamp() const;
    // 主机字节序的时间戳，已经转换为毫秒
    uint64_t getStampMS(bool ntp = true) const;
    // 主机字节序的ssrc
    uint32_t getSSRC() const;
    // 有效负载，跳过csrc、ext
    uint8_t *getPayload();
    // 有效负载长度，不包括csrc、ext、padding
    size_t getPayloadSize() const;

	// 音视频类型
    TrackType type;

	//音视频采样率
	uint32_t sample_rate;

	//ntp时间戳
	uint64_t ntp_stamp_ms;

	int track_index;

	static Ptr create();

	static constexpr int kRtpVersion = 2;
    static constexpr int kRtpHeaderSize = 12;
    static constexpr int kRtpTcpHeaderSize = 4;
	static constexpr int kRtpMaxSize = 2;

private:
	RtpPacket() = default;
};

#endif