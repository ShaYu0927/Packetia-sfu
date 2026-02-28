#include "UdpSession.h"

namespace network
{
void UdpSession::OnStun(const network::SocketAddr& src, const uint8_t* d, size_t n)
{

}

void UdpSession::OnDtls(const network::SocketAddr& src, const uint8_t* d, size_t n)
{

}

void UdpSession::OnRtp (const network::SocketAddr& src, const uint8_t* d, size_t n)
{

}

void UdpSession::OnRtcp(const network::SocketAddr& src, const uint8_t* d, size_t n)
{

}

void UdpSession::Tick(uint64_t now_ms)
{

}

bool UdpSession::SendTo(const network::SocketAddr& dst, const uint8_t* data, size_t len)
{

}

bool UdpSession::IsSelectedPeer(const network::SocketAddr &src) const
{
    return has_selected_ && (src == selected_peer_);
}

void UdpSession::SetSelectedPeer(const network::SocketAddr &peer)
{
    selected_peer_ = peer;
    has_selected_ = true;
    if (on_selected_peer_) 
    {
        on_selected_peer_(shared_from_this(), selected_peer_);
    }

}

void UdpMuxHandler::OnDatagram(const network::SocketAddr& src,
                            const uint8_t* data,
                            size_t len)
{
    if (!data || len == 0) return;
    const auto proto = DetectProto(data, len);

    std::shared_ptr<UdpSession> sess;


}

inline bool UdpMuxHandler::IsStunPacket(const uint8_t *d, size_t n)
{
    return false;
}

inline bool UdpMuxHandler::IsDtlsPacket(const uint8_t *d, size_t n)
{
    return false;
}

inline bool UdpMuxHandler::IsRtcpPacket(const uint8_t *d, size_t n)
{
    return false;
}

inline UdpMuxHandler::UdpProto UdpMuxHandler::DetectProto(const uint8_t *d, size_t n)
{
    if (IsStunPacket(d, n)) return UdpProto::Stun;
    if (IsDtlsPacket(d, n)) return UdpProto::Dtls;
    if (IsRtcpPacket(d, n)) return UdpProto::Rtcp;
    return UdpProto::Unknown;
}

void UdpMuxHandler::BindPeer(const network::SocketAddr &peer, const std::shared_ptr<UdpSession> &sess)
{
    peer_map_[peer] = sess;
}

void UdpMuxHandler::UnbindPeer(const network::SocketAddr &peer)
{
    peer_map_.erase(peer);
}

std::shared_ptr<UdpSession> UdpMuxHandler::FindByPeer(const network::SocketAddr &src)
{
    auto it = peer_map_.find(src);
    if (it == peer_map_.end()) return nullptr;
    auto sp = it->second.lock();
    if (!sp) peer_map_.erase(it); 
    return sp;
}

std::shared_ptr<UdpSession> MediaEngine::CreateSession(const std::string &session_id)
{
    auto sess = std::make_shared<UdpSession>(udp_.get());
    sess->SetOnSelectedPeer([this](std::shared_ptr<UdpSession> s, const network::SocketAddr& peer) {
        handler_->BindPeer(peer, s);
    });
    return sess;
}

}