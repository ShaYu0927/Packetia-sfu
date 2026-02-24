#ifndef _UDPSESSION_H_
#define _UDPSESSION_H_

#include "UdpServer.h"

namespace network
{

class UdpSession 
{
public:
    explicit UdpSession(network::UdpServer* udp) : udp_(udp) {}

    void OnStun(const network::SocketAddr& src, const uint8_t* d, size_t n);
    void OnDtls(const network::SocketAddr& src, const uint8_t* d, size_t n);
    void OnRtp (const network::SocketAddr& src, const uint8_t* d, size_t n);
    void OnRtcp(const network::SocketAddr& src, const uint8_t* d, size_t n);

    void Tick(uint64_t now_ms);

    bool SendTo(const network::SocketAddr& dst, const uint8_t* data, size_t len);

    bool IsSelectedPeer(const network::SocketAddr& src) const;
    void SetSelectedPeer(const network::SocketAddr& peer);
    const network::SocketAddr& SelectedPeer() const;

private:
    network::UdpServer* udp_;
    std::string session_id_;

    network::SocketAddr selected_peer_;
    bool has_selected_{false};

    uint64_t last_rx_ms_{0};
    uint64_t last_stun_ms_{0};


};

class UdpMuxHandler : public IUdpHandler 
{
public:

    explicit UdpMuxHandler(network::UdpServer* udp) : udp_(udp) {}

    void OnDatagram(const network::SocketAddr& src,
                            const uint8_t* data,
                            size_t len) override
                            {
                                
                            }

private:
    network::UdpServer* udp_; // 非 owning
    std::unordered_map<std::string, std::shared_ptr<UdpSession>> sessions_;
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