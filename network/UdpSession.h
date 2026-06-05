#ifndef _UDPSESSION_H_
#define _UDPSESSION_H_

#include "UdpServer.h"
#include "EndpointBase.h"
#include "IceAgent.h"
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace network
{

class UdpSession : public utils::EndpointBase,
                   public std::enable_shared_from_this<UdpSession>
{
public:
    using OnSelectedPeerFn = std::function<void(std::shared_ptr<UdpSession>,
                                               const network::SocketAddr&)>;

    UdpSession(std::uint64_t endpoint_id,
               std::string name,
               network::UdpServer* udp)
        : EndpointBase(endpoint_id, std::move(name)),
          udp_(udp)
    {
    }

    bool Start() override;
    void Stop() override;

public:
    void OnStun(WorkJob& job) override;
    void OnDtls(WorkJob& job) override;
    void OnRtp(WorkJob& job) override;
    void OnRtcp(WorkJob& job) override;

public:
    void ConfigureIce(std::string local_ufrag,
                      std::string local_pwd,
                      std::string remote_ufrag = {},
                      std::string remote_pwd = {});

    bool HandleStunDatagram(const network::SocketAddr& src,
                            const uint8_t* data,
                            size_t len);

    bool SendTo(const network::SocketAddr& dst, const uint8_t* data, size_t len);

    bool IsSelectedPeer(const network::SocketAddr& src) const;
    void SetSelectedPeer(const network::SocketAddr& peer);
    const network::SocketAddr& SelectedPeer() const;

    void SetOnSelectedPeer(OnSelectedPeerFn cb) { on_selected_peer_ = std::move(cb); }

private:
    network::UdpServer* udp_;
    std::string session_id_;

    network::SocketAddr selected_peer_;
    bool has_selected_{false};

    uint64_t last_rx_ms_{0};
    uint64_t last_stun_ms_{0};

    ice::IceAgent ice_agent_;
    OnSelectedPeerFn on_selected_peer_;
};

class UdpMuxHandler : public IUdpHandler 
{
public:
    enum class UdpProto { Stun, Dtls, Rtp, Rtcp, Unknown };

    explicit UdpMuxHandler(network::UdpServer* udp) : udp_(udp) {}

    void OnDatagram(const network::SocketAddr& src,
                            const uint8_t* data,
                            size_t len) override;

    static inline bool IsStunPacket(const uint8_t* d, size_t n);
    static inline bool IsDtlsPacket(const uint8_t* d, size_t n);
    static inline bool IsRtcpPacket(const uint8_t* d, size_t n);
    static inline bool IsRtpPacket(const uint8_t* d, size_t n);
    static inline UdpProto DetectProto(const uint8_t* d, size_t n);


    void BindPeer(const network::SocketAddr& peer, const std::shared_ptr<UdpSession>& sess);
    void UnbindPeer(const network::SocketAddr& peer);
    std::shared_ptr<UdpSession> FindByPeer(const network::SocketAddr& src);

    void SetPendingSession(const std::shared_ptr<UdpSession>& sess);
    void ClearPendingSession();

    inline WorkType ToWorkJobType(UdpMuxHandler::UdpProto proto)
    {
        switch (proto)
        {
            case UdpMuxHandler::UdpProto::Stun: return WorkType::Stun;
            case UdpMuxHandler::UdpProto::Dtls: return WorkType::Dtls;
            case UdpMuxHandler::UdpProto::Rtp:  return WorkType::Rtp;
            case UdpMuxHandler::UdpProto::Rtcp: return WorkType::Rtcp;
            default:                            return WorkType::Invalid;
        }
    }



private:
    network::UdpServer* udp_;
    std::unordered_map<network::SocketAddr,
                   std::weak_ptr<network::UdpSession>,
                   network::SocketAddrHash> peer_map_;
    
    std::weak_ptr<UdpSession> pending_session_;  /* stun binding session */
};

class MediaEngine 
{
public:
    explicit MediaEngine(EventLoop* loop);

    bool Start(const std::string& ip, uint16_t port);
    void Stop();
    std::shared_ptr<UdpSession> CreateSession(const std::string& session_id);
    void Tick(uint64_t now_ms);


    std::unique_ptr<network::UdpServer> udp_;
    std::shared_ptr<UdpMuxHandler> handler_;
};

}


#endif /* _UDPSESSION_H_ */
