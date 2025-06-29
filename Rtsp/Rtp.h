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


typedef struct _RTP_header
{
#if RTP_HEADER_BIG_ENDIAN
	/* 大端序 */
	unsigned char version   : 2;
	unsigned char padding   : 1;
	unsigned char extension : 1;
	unsigned char csrc      : 4;
	unsigned char marker    : 1;
	unsigned char payload   : 7;
#else
	/* 小端序 */
	unsigned char csrc      : 4;
	unsigned char extension : 1;
	unsigned char padding   : 1;
	unsigned char version   : 2;
	unsigned char payload   : 7;
	unsigned char marker    : 1;
#endif 
	unsigned short seq;
	unsigned int   ts;
	unsigned int   ssrc;
} RtpHeader;

struct RtpPacket
{
    RtpPacket()
		: data(new uint8_t[1600], std::default_delete<uint8_t[]>())
	{
		type = 0;
		size = 0;
		timestamp = 0;
		last = 0;
	}
    std::shared_ptr<uint8_t> data;
	uint32_t size;
	uint32_t timestamp;
	uint8_t  type;
	uint8_t  last;
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

#endif