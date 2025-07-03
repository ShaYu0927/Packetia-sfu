#ifndef _RTPCONNECTION_H_
#define _RTPCONNECTION_H_

#include <memory>
#include <random>
#include <vector>
#include "TcpConnection.h"
#include "Rtp.h"
#include "Media.h"


class RtspConnection;

class RtpConnection
{
public:
    RtpConnection(std::weak_ptr<TcpConnection> rtsp_connection);
    virtual ~RtpConnection();
    
    uint32_t GetRtpSessionId() const
    { return (uint32_t)((size_t)(this)); }

    void SetClockrate(MediaChannelId channel_id, uint32_t clock_rate);

    void SetPlayLoadType(MediaChannelId channel_id, uint8_t payload_type);

    bool SetupRtpOverTcp(MediaChannelId channel_id, uint16_t rtp_channel, uint16_t rtcp_channel);
    bool SetupRtpOverUdp(MediaChannelId channel_id, uint16_t rtp_port, uint16_t rtcp_port);
    bool SetupRtpOverMulticast(MediaChannelId channel_id, const std::string& ip, uint16_t port);

    void SetFrameType(uint8_t frame_type);
    

    uint16_t GetLocalRtpPort(MediaChannelId channel_id) const {
        return local_rtp_port_[channel_id];
    }
    uint16_t GetLocalRtcpPort(MediaChannelId channel_id) const {
        return local_rtcp_port_[channel_id];
    }
private:
    friend class RtspConnection;
    friend class MediaSession;

    void SetRtpHeader(MediaChannelId channel_id, RtpPacket& pkt);
    int SentRtpPacket(MediaChannelId channel_id, RtpPacket pkt);
    int  SendRtpOverTcp(MediaChannelId channel_id, RtpPacket pkt);
    int  SendRtpOverUdp(MediaChannelId channel_id, RtpPacket pkt);

    std::weak_ptr<TcpConnection> rtsp_connection_;
    std::string rtsp_ip_;
    uint16_t rtsp_port_;
    RTPTransportMode transport_mode_;

    bool is_multicast_;

    bool is_closed_;
    bool has_key_frame_ = false;

    uint8_t  frame_type_ = 0;
    uint16_t local_rtp_port_[MAX_MEDIA_CHANNEL];
    uint16_t local_rtcp_port_[MAX_MEDIA_CHANNEL];

    int rtpfd_[MAX_MEDIA_CHANNEL];
    int rtcpfd_[MAX_MEDIA_CHANNEL];


    struct sockaddr_in peer_addr_;
    struct sockaddr_in peer_rtp_addr_[MAX_MEDIA_CHANNEL];
    struct sockaddr_in peer_rtcp_sddr_[MAX_MEDIA_CHANNEL];

    std::vector<MediaChannelInfo> media_channels_;   //存储音频和视频的通道信息
};


#endif // _RTPCONNECTION_H_