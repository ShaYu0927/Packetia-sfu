#ifndef _ICE_AGENT_H_
#define _ICE_AGENT_H_

#include "IceContext.h"
#include "UdpSocket.h"
#include "stun/Stun.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ice
{

class IceAgent
{
public:
    enum class HandleResult
    {
        NotStun,
        Ignored,
        ErrorResponse,
        SuccessResponse
    };

    using SelectedPeerCallback = std::function<void(const network::SocketAddr&)>;

public:
    IceAgent();

    void SetLocalCredentials(std::string ufrag, std::string pwd);
    void SetRemoteCredentials(std::string ufrag, std::string pwd);
    void SetRole(IceContext::Role role);
    void SetTieBreaker(uint64_t tie_breaker);
    void SetOnSelectedPeer(SelectedPeerCallback cb);

    const IceContext& Context() const { return ctx_; }

    HandleResult HandleDatagram(const network::SocketAddr& src,
                                const uint8_t* data,
                                size_t len,
                                std::vector<uint8_t>& response);

    bool HasSelectedPeer() const { return has_selected_peer_; }
    const network::SocketAddr& SelectedPeer() const { return selected_peer_; }

private:
    bool HandleBindingRequest(const network::SocketAddr& src,
                              const protocol::StunMessageInfo& msg,
                              std::vector<uint8_t>& response,
                              HandleResult& result);

    bool BuildError(const protocol::StunMessageInfo& msg,
                    uint16_t code,
                    const std::string& reason,
                    std::vector<uint8_t>& response);

    bool BuildSuccess(const network::SocketAddr& src,
                      const protocol::StunMessageInfo& msg,
                      std::vector<uint8_t>& response);

    bool ValidateUsername(const protocol::StunMessageInfo& msg,
                          std::string& remote_ufrag);

    bool HasRoleConflict(const protocol::StunMessageInfo& msg) const;
    static bool SocketAddrToIpEndpoint(const network::SocketAddr& src,
                                       protocol::IpEndpoint& out);

private:
    IceContext ctx_;
    network::SocketAddr selected_peer_{};
    bool has_selected_peer_{false};
    SelectedPeerCallback on_selected_peer_;
};

} // namespace ice

#endif /* _ICE_AGENT_H_ */
