#include "RtpConnection.h"

RtpConnection::RtpConnection(std::weak_ptr<TcpConnection> rtsp_connection)
    :rtsp_connection_(rtsp_connection)
{
    //初始化媒体通道端口
    std::random_device rd;
    media_channels_.resize(MAX_MEDIA_CHANNEL);
    for(int i = 0; i < MAX_MEDIA_CHANNEL; ++i) {
        local_rtp_port_[i] = 0;
        local_rtcp_port_[i] = 0;

        media_channels_[i].rtp_header.version = RTP_VERSION;
        media_channels_[i].packet_seq = rd() & 0xffff;
        media_channels_[i].rtp_header.seq = 0;
        media_channels_[i].rtp_header.ts = htonl(rd());
        media_channels_[i].rtp_header.ssrc = htonl(rd());
    }
    
    auto conn = rtsp_connection.lock();
    if (conn) 
    {
        rtsp_ip_ = conn->GetIp();
        rtsp_port_ = conn->GetPort();
    } 
    else 
    {
        rtsp_ip_ = "";
    }
}

RtpConnection::~RtpConnection()
{
    for(int i = 0;i < MAX_MEDIA_CHANNEL;++i)
    {
        if(rtpfd_[i] > 0)
        {
            SocketUtil::Close(rtpfd_[i]);
        }
        if(rtcpfd_[i] > 0) 
        {
			SocketUtil::Close(rtcpfd_[i]);
		}
    }

}

void RtpConnection::SetClockrate(MediaChannelId channel_id, uint32_t clock_rate)
{
}

void RtpConnection::SetPlayLoadType(MediaChannelId channel_id, uint8_t payload_type)
{
}
/*
    SETUP rtsp://xxx/stream/trackID=0 RTSP/1.0
    Transport: RTP/AVP/TCP;unicast;interleaved=0-1

*/

bool RtpConnection::SetupRtpOverTcp(MediaChannelId channel_id, uint16_t rtp_channel, uint16_t rtcp_channel)
{
    auto conn = rtsp_connection_.lock();
    if (!conn) {
        return false; // RTSP connection is not available
    }
    media_channels_[channel_id].rtp_channel = rtp_channel;
    media_channels_[channel_id].rtcp_channel = rtcp_channel;
    media_channels_[channel_id].is_setup = true;
    rtpfd_[channel_id] = conn->GetSocket();
	rtcpfd_[channel_id] = conn->GetSocket();
    transport_mode_ = RTPTransportMode::RTP_OVER_TCP;
    return true;
}

/*
    客户端：SETUP ... Transport: RTP/AVP;unicast;client_port=5000-5001

    服务器：
    - 打开 UDP socket 绑定 4xxxx/4xxx+1
    - 记录远端地址  -> 客户端 IP + 5000/5001
    - 设置 channel 状态：is_setup = true
    - transport_mode_ = RTP_OVER_UDP
*/
bool RtpConnection::SetupRtpOverUdp(MediaChannelId channel_id, uint16_t rtp_port, uint16_t rtcp_port)
{
    auto conn = rtsp_connection_.lock();
    if (!conn) {
        return false; // RTSP connection is not available
    }
    if(SocketUtil::GetPeerAddr(conn->GetSocket(), &peer_addr_) < 0) 
    {
		return false;
	}
    media_channels_[channel_id].rtp_port = rtp_port;
    media_channels_[channel_id].rtcp_port = rtcp_port;
    std::random_device rd;
    for(int n = 0; n < 10; ++n) 
    {
       if (n == 10) 
       {
			return false;
	   }
       local_rtp_port_[channel_id] = rd() & 0xfffe;
	   local_rtcp_port_[channel_id] =local_rtp_port_[channel_id] + 1;

       // 创建 RTP socket与 RTCP socket

       rtpfd_[channel_id] = ::socket(AF_INET, SOCK_DGRAM, 0);
       if(!SocketUtil::Bind(rtpfd_[channel_id], "0.0.0.0",  local_rtp_port_[channel_id])) {
			SocketUtil::Close(rtpfd_[channel_id]);
			continue;
		}

        rtcpfd_[channel_id] = ::socket(AF_INET, SOCK_DGRAM, 0);

        if(!SocketUtil::Bind(rtcpfd_[channel_id], "0.0.0.0", local_rtcp_port_[channel_id]))
        {
            SocketUtil::Close(rtcpfd_[channel_id]);
            SocketUtil::Close(rtpfd_[channel_id]);
            continue;
        }
        break;
    }

    SocketUtil::SetSendBufSize(rtpfd_[channel_id], 50*1024);

    peer_rtp_addr_[channel_id].sin_family = AF_INET;
	peer_rtp_addr_[channel_id].sin_addr.s_addr = peer_addr_.sin_addr.s_addr;
	peer_rtp_addr_[channel_id].sin_port = htons(media_channels_[channel_id].rtp_port);

    peer_rtcp_sddr_[channel_id].sin_family = AF_INET;
    peer_rtcp_sddr_[channel_id].sin_addr.s_addr = peer_addr_.sin_addr.s_addr;
    peer_rtcp_sddr_[channel_id].sin_port = htons(media_channels_[channel_id].rtcp_port);

    media_channels_[channel_id].is_setup = true;
	transport_mode_ = RTP_OVER_UDP;

    return true;
}

bool RtpConnection::SetupRtpOverMulticast(MediaChannelId channel_id, std::string ip, uint16_t port)
{
    std::random_device rd;
    for(int n = 0;n < 10;n++)
    {
        if(n == 10) 
        {
            return false; // Failed to bind multicast address
        }
        local_rtp_port_[channel_id] = rd() & 0xfffe;
        rtpfd_[channel_id] = ::socket(AF_INET, SOCK_DGRAM, 0);
        if(!SocketUtil::Bind(rtpfd_[channel_id], ip, local_rtp_port_[channel_id])) 
        {
            SocketUtil::Close(rtpfd_[channel_id]);
            continue;
        }
        break;
    }

    media_channels_[channel_id].rtp_port = port;

    peer_rtp_addr_[channel_id].sin_family = AF_INET;
	peer_rtp_addr_[channel_id].sin_addr.s_addr = inet_addr(ip.c_str());
	peer_rtp_addr_[channel_id].sin_port = htons(port);

    media_channels_[channel_id].is_setup = true;
	transport_mode_ = RTP_OVER_MULTICAST;
	is_multicast_ = true;
	return true;
}

void RtpConnection::SetFrameType(uint8_t frameType)
{
}

void RtpConnection::SetRtpHeader(MediaChannelId channel_id, RtpPacket pkt)
{
}

int RtpConnection::SendRtpOverTcp(MediaChannelId channel_id, RtpPacket pkt)
{
    return 0;
}

int RtpConnection::SendRtpOverUdp(MediaChannelId channel_id, RtpPacket pkt)
{
    return 0;
}
