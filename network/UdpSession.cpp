#include "UdpSession.h"
#include "logger.h"
#include "TimeUtil.h"


namespace network
{

using HandlerFn = void (*)(WorkJob&, void*);
inline UdpSession* GetSession(WorkerContext* ctx, uint64_t key)
{
    auto it = ctx->sessions.find(key);
    if (it == ctx->sessions.end())
        return nullptr;

    return static_cast<UdpSession*>(it->second.ptr);
}

template<void (UdpSession::*Fn)(WorkJob&)>
static void Handle(WorkJob& job, void* ctx)
{
    auto* worker = static_cast<WorkerContext*>(ctx);
    auto* sess = GetSession(worker, job.key);
    if (!sess) return;

    (sess->*Fn)(job);
}

static HandlerFn HandleStun = Handle<&UdpSession::OnStun>;
static HandlerFn HandleDtls = Handle<&UdpSession::OnDtls>;
static HandlerFn HandleRtp  = Handle<&UdpSession::OnRtp>;
static HandlerFn HandleRtcp = Handle<&UdpSession::OnRtcp>;

static HandlerFn DispatchHandler(UdpMuxHandler::UdpProto proto)
{
    switch (proto)
    {
    case UdpMuxHandler::UdpProto::Stun: return HandleStun;
    case UdpMuxHandler::UdpProto::Dtls: return HandleDtls;
    case UdpMuxHandler::UdpProto::Rtp:  return HandleRtp;
    case UdpMuxHandler::UdpProto::Rtcp: return HandleRtcp;
    default: return nullptr;
    }
}

bool UdpSession::Start()
{
    SetState(State::kRunning);
    return true;
}

void UdpSession::Stop()
{
    SetState(State::kStopped);
}

void UdpSession::OnStun(WorkJob& job)
{
    LOG_INFO("UdpSessionEndpoint::onStun",job.key);
}

void UdpSession::OnDtls(WorkJob& job)
{
    LOG_INFO("UdpSessionEndpoint::OnDtls",job.key);
}

void UdpSession::OnRtp(WorkJob& job)
{
    LOG_INFO("UdpSessionEndpoint::OnRtp",job.key);
}

void UdpSession::OnRtcp(WorkJob& job)
{
    LOG_INFO("UdpSessionEndpoint::OnRtcp",job.key);
}

bool UdpSession::SendTo(const network::SocketAddr& dst, const uint8_t* data, size_t len)
{
    return udp_ && udp_->SendTo(dst, data, len);
}

void UdpSession::ConfigureIce(std::string local_ufrag,
                              std::string local_pwd,
                              std::string remote_ufrag,
                              std::string remote_pwd)
{
    ice_agent_.SetLocalCredentials(std::move(local_ufrag), std::move(local_pwd));
    if (!remote_ufrag.empty() || !remote_pwd.empty())
    {
        ice_agent_.SetRemoteCredentials(std::move(remote_ufrag), std::move(remote_pwd));
    }
    ice_agent_.SetOnSelectedPeer([this](const network::SocketAddr& peer) {
        SetSelectedPeer(peer);
    });
}

bool UdpSession::HandleStunDatagram(const network::SocketAddr& src,
                                    const uint8_t* data,
                                    size_t len)
{
    std::vector<uint8_t> response;
    const auto result = ice_agent_.HandleDatagram(src, data, len, response);
    if (result == ice::IceAgent::HandleResult::NotStun)
    {
        return false;
    }

    last_rx_ms_ = Timestamp::NowMs();
    last_stun_ms_ = last_rx_ms_;

    if (!response.empty() && !SendTo(src, response.data(), response.size()))
    {
        LOG_ERROR("[ICE] failed to send STUN response, peer=", src.ToString(),
                  " size=", response.size());
    }

    LOG_INFO("[ICE] handled STUN datagram, peer=", src.ToString(),
             " result=", static_cast<int>(result),
             " response_size=", response.size());
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
    }

    if (!sess)
    {
        LOG_ERROR("udp session not found, proto=", static_cast<int>(proto), " len=", len);
        return;
    }

    if (proto == UdpProto::Stun)
    {
        sess->HandleStunDatagram(src, data, len);
        return;
    }

    auto endpoint_id = sess->Id();
    Packet* pkt = PacketPool::instance().acquire();
    if (!pkt)
    {
        LOG_ERROR("PacketPool acquire failed, endpoint_id=", endpoint_id);
        return;
    }
    pkt->recv_ts = Timestamp::NowMs();
    pkt->assign(data, len);

    WorkJob job{};
    job.key  = endpoint_id;
    job.type = ToWorkJobType(proto);
    job.pkt  = pkt;
    job.enqueue_ts = pkt->recv_ts;
    job.handler = DispatchHandler(proto);
    job.deleter = [](WorkJob& job) {
        WorkerService::realse(job.pkt);
        job.pkt = nullptr;
    };

    WorkerService::post("endpoint_pool", std::move(job));
  
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

inline bool UdpMuxHandler::IsRtpPacket(const uint8_t *d, size_t n)
{
    if (!d || n < 12) return false;
    if ((d[0] >> 6) != 2) return false;
    return !IsRtcpPacket(d, n);
}

inline UdpMuxHandler::UdpProto UdpMuxHandler::DetectProto(const uint8_t *d, size_t n)
{
    if (IsStunPacket(d, n)) return UdpProto::Stun;
    if (IsDtlsPacket(d, n)) return UdpProto::Dtls;
    if (IsRtcpPacket(d, n)) return UdpProto::Rtcp;
    if (IsRtpPacket(d, n)) return UdpProto::Rtp;
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
    std::uint64_t endpoint_id = std::hash<std::string>{}(session_id);
    auto sess = std::make_shared<UdpSession>(endpoint_id, session_id, udp_.get());
    sess->ConfigureIce(session_id, session_id + "_ice_pwd");

    sess->SetOnSelectedPeer([this](std::shared_ptr<UdpSession> s, const network::SocketAddr& peer) {
        handler_->BindPeer(peer, s);
    });
    return sess;
}

}
