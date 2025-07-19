#include "RtpConnection.h"

RtpConnection::RtpConnection(std::weak_ptr<TcpConnection> rtsp_connection)
    :rtsp_connection_(rtsp_connection)
{
    std::random_device rd;
    std::mt19937 gen(rd()); // 更强的随机生成器
    std::uniform_int_distribution<uint16_t> seq_dist(0, 0xFFFF);
    std::uniform_int_distribution<uint32_t> ts_dist(0, 0xFFFFFFFF);
    std::uniform_int_distribution<uint16_t> port_dist(10000, 50000); // RTP/RTCP 端口范围

    media_channels_.resize(MAX_MEDIA_CHANNEL);

    for (int i = 0; i < MAX_MEDIA_CHANNEL; ++i) {
        // 初始化端口号为偶数，rtcp 为 rtp+1
        uint16_t base_port = port_dist(gen) & 0xFFFE;
        local_rtp_port_[i] = base_port;
        local_rtcp_port_[i] = base_port + 1;

        auto& channel = media_channels_[i];
        channel.rtp_header.setVersion(RTP_VERSION);
        channel.rtp_sequence = seq_dist(gen);
        channel.rtp_header.setSequence(channel.rtp_sequence);
        channel.rtp_header.setTimestamp(ts_dist(gen));
        channel.rtp_header.setSSRC(ts_dist(gen));

        channel.transport.transport_type = MediaTransportType::UDP; // 默认UDP
        channel.state = MediaSessionState::Init;
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
        return false; // RTSP connection lost
    }

    if (channel_id >= media_channels_.size()) {
        return false; // Invalid channel
    }

    auto& channel = media_channels_[channel_id];
    channel.transport.transport_type = MediaTransportType::TCP;
    channel.transport.tcp.rtp_channel = rtp_channel;
    channel.transport.tcp.rtcp_channel = rtcp_channel;

    channel.markSetup(); // 设置状态标志

    int socket_fd = conn->GetSocket();
    rtpfd_[channel_id] = socket_fd;
    rtcpfd_[channel_id] = socket_fd;

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
        return false;
    }

    if (channel_id >= media_channels_.size()) {
        return false;
    }

    if (SocketUtil::GetPeerAddr(conn->GetSocket(), &peer_addr_) < 0) {
        return false;
    }

    auto& channel = media_channels_[channel_id];
    channel.transport.transport_type = MediaTransportType::UDP;
    channel.transport.udp.rtp_port = rtp_port;
    channel.transport.udp.rtcp_port = rtcp_port;

    std::random_device rd;
    bool bind_success = false;

    for (int n = 0; n < 10; ++n) {
        uint16_t base_port = (rd() & 0xfffe) | 0x2000;  // 起始端口避免过低
        local_rtp_port_[channel_id] = base_port;
        local_rtcp_port_[channel_id] = base_port + 1;

        rtpfd_[channel_id] = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (!SocketUtil::Bind(rtpfd_[channel_id], "0.0.0.0", base_port)) {
            SocketUtil::Close(rtpfd_[channel_id]);
            continue;
        }

        rtcpfd_[channel_id] = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (!SocketUtil::Bind(rtcpfd_[channel_id], "0.0.0.0", base_port + 1)) {
            SocketUtil::Close(rtcpfd_[channel_id]);
            SocketUtil::Close(rtpfd_[channel_id]);
            continue;
        }

        bind_success = true;
        break;
    }

    if (!bind_success) {
        return false;
    }

    SocketUtil::SetSendBufSize(rtpfd_[channel_id], 50 * 1024);

    // 设置对端地址
    peer_rtp_addr_[channel_id] = {
        .sin_family = AF_INET,
        .sin_port = htons(rtp_port),
        .sin_addr = peer_addr_.sin_addr,
    };
    peer_rtcp_sddr_[channel_id] = {
        .sin_family = AF_INET,
        .sin_port = htons(rtcp_port),
        .sin_addr = peer_addr_.sin_addr,
    };

    channel.markSetup();
    transport_mode_ = RTP_OVER_UDP;

    // InfoL << "Setup RTP over UDP: channel_id=" << channel_id
    //       << ", local_rtp_port=" << local_rtp_port_[channel_id]
    //       << ", peer_rtp_port=" << rtp_port;

    return true;
}


bool RtpConnection::SetupRtpOverMulticast(MediaChannelId channel_id, const std::string& ip, uint16_t port)
{
    if (channel_id >= media_channels_.size()) {
        return false;
    }

    std::random_device rd;
    bool bind_success = false;

    for (int n = 0; n < 10; ++n) {
        uint16_t local_port = (rd() & 0xfffe) | 0x2000;
        local_rtp_port_[channel_id] = local_port;

        rtpfd_[channel_id] = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (rtpfd_[channel_id] < 0) {
            continue;
        }

        if (!SocketUtil::Bind(rtpfd_[channel_id], "0.0.0.0", local_port)) {
            SocketUtil::Close(rtpfd_[channel_id]);
            continue;
        }

        // 加入 multicast group
        struct ip_mreq mreq;
        mreq.imr_multiaddr.s_addr = inet_addr(ip.c_str());
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);

        if (setsockopt(rtpfd_[channel_id], IPPROTO_IP, IP_ADD_MEMBERSHIP, (char*)&mreq, sizeof(mreq)) < 0) {
            SocketUtil::Close(rtpfd_[channel_id]);
            continue;
        }

        bind_success = true;
        break;
    }

    if (!bind_success) {
        return false;
    }

    auto& channel = media_channels_[channel_id];
    channel.transport.transport_type = MediaTransportType::UDP;
    channel.transport.udp.rtp_port = port;

    // 目标地址设置
    peer_rtp_addr_[channel_id] = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr = { .s_addr = inet_addr(ip.c_str()) }
    };

    channel.markSetup(); // 使用状态标志更优
    transport_mode_ = RTP_OVER_MULTICAST;
    is_multicast_ = true;

    // InfoL << "Setup RTP over Multicast: channel_id=" << channel_id
    //       << ", local_port=" << local_rtp_port_[channel_id]
    //       << ", group_ip=" << ip
    //       << ", group_port=" << port;

    return true;
}

void RtpConnection::SetFrameType(uint8_t frameType)
{
    frame_type_ = frameType;
    for (auto& channel : media_channels_) {
        channel.rtp_header.setPayloadType(frameType);
    }

    //标记在这个会话里面是否已经收到了关键帧
    if(!has_key_frame_ && (frameType == 0 || frameType == FRAME_TYPE_I))
    {
        has_key_frame_ = true;
    }
}

int RtpConnection::StartSendRTP(uint32_t timestamp)
{
    return 0;
}

int RtpConnection::StartSendRTCP()
{
    return 0;
}

void RtpConnection::SetRtpHeader(MediaChannelId channel_id, RtpPacket &pkt)
{
    auto& channel = media_channels_[channel_id];

    // 若未播放或未录制或尚未有关键帧，丢弃
    if (!(channel.isPlay() || channel.isRecord()) || !has_key_frame_) {
        pkt.size = 0;
        pkt.data.reset();
        return;
    }

    // 设置 RTP header
    channel.rtp_header.setMarker(pkt.getMarker());
    channel.rtp_header.setTimestamp(pkt.getStamp());
    channel.rtp_header.setSequence(channel.rtp_sequence++);

    // 将 RTP header 序列化为网络字节序并写入 packet 前部
    if (pkt.data && pkt.size >= RtpHeader::kSize) {
        channel.rtp_header.serialize(pkt.data.get());
    } else {
        // 安全检查不通过，丢弃
        pkt.size = 0;
        pkt.data.reset();
    }
}


int RtpConnection::SentRtpPacket(MediaChannelId channel_id, RtpPacket pkt)
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
