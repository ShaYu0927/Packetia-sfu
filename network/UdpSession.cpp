#include "UdpSession.h"
#include "Stun.h"

namespace network
{
void UdpSession::OnStun(const network::SocketAddr& src, const uint8_t* d, size_t n)
{
    protocol::StunMessageInfo req{};
    if (!protocol::StunCodec::Parse(d, n, req)) return;

    if (req.method != protocol::StunMethod::Binding || req.klass != protocol::StunClass::Request) return;

    if (!IsSelectedPeer(src)) SetSelectedPeer(src);

    protocol::IpEndpoint ep{};

    if (src.IsV4()) 
    {
        ep.family = 4;
        ep.port = src.Port();
        auto v4 = src.IPv4Bytes();              
        std::memcpy(ep.ip.data(), v4.data(), 4);
    } 
    else 
    {
        ep.family = 6;
        ep.port = src.Port();
        auto v6 = src.IPv6Bytes();
        std::memcpy(ep.ip.data(), v6.data(), 16);
    }

    uint8_t out[256];
    size_t out_len = 0;
    if (protocol::StunCodec::BuildBindingSuccess(req, ep, out, sizeof(out), out_len))
        SendTo(src, out, out_len);
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
    return true;
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

void UdpMuxHandler::SetPendingSession(const std::shared_ptr<UdpSession>& sess)
{
    pending_session_ = sess;
}

void UdpMuxHandler::ClearPendingSession()
{
    pending_session_.reset();
}

void UdpMuxHandler::OnDatagram(const network::SocketAddr& src,
                            const uint8_t* data,
                            size_t len)
{
    if (!data || len == 0) return;

    auto sess = FindByPeer(src);
    const auto proto = DetectProto(data, len);

    if(!sess)
    {
        if(proto == UdpProto::Stun)
        {
            sess = pending_session_.lock();
        }

        if (!sess)
            return;
    }

    switch (proto)
    {
    case UdpProto::Stun: sess->OnStun(src, data, len); break;
    case UdpProto::Dtls: sess->OnDtls(src, data, len); break;
    case UdpProto::Rtcp: sess->OnRtcp(src, data, len); break;
    default:             sess->OnRtp (src, data, len); break;
    }
}

/* 
    RFC 7983 
    https://www.rfc-editor.org/pdfrfc/rfc7983.txt.pdf
*/
inline bool UdpMuxHandler::IsStunPacket(const uint8_t *d, size_t n)
{
    if (!d || n < 20) return false;
    if ((d[0] & 0xC0) != 0x00) return false;
    const uint32_t mc = (uint32_t(d[4]) << 24) | (uint32_t(d[5]) << 16) | (uint32_t(d[6]) << 8) | uint32_t(d[7]);
    return mc == 0x2112A442;
}

inline bool UdpMuxHandler::IsDtlsPacket(const uint8_t *d, size_t n)
{
    if (!d || n < 13) return false; 
    const uint8_t ct = d[0];
    if (ct < 20 || ct > 23) return false;
    if (d[1] != 0xFE) return false;
    if (d[2] != 0xFD && d[2] != 0xFF) return false;
    return true;
}

inline bool UdpMuxHandler::IsRtcpPacket(const uint8_t *d, size_t n)
{
    if (!d || n < 4) return false;
    if ((d[0] >> 6) != 2) return false;
    const uint8_t pt = d[1];
    if (pt >= 192 && pt <= 223) return true; 
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

    auto pending = pending_session_.lock();
    if (pending && pending == sess)
    {
        pending_session_.reset();
    }
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