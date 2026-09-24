#ifndef _ICE_AGENT_H_
#define _ICE_AGENT_H_

#include "IceContext.h"
#include "UdpSocket.h"
#include "stun/Stun.h"
#include "StateController.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ice
{

class IceAgent
{
public:
    enum class State { New, Checking, Completed, Failed, Closed };
    State CurrentState() const noexcept { return lifecycle_.CurrentState(); }
    // Passive ICE responder used by the SFU's ICE-Lite session. Call on the
    // session event loop; this is not a full ICE checklist/transaction engine.
    enum class HandleResult
    {
        NotStun,
        Ignored,
        ErrorResponse,
        SuccessResponse
    };

    using SelectedPeerCallback = std::function<void(const network::SocketAddr&)>;

public:
    using Clock = std::function<uint64_t()>; // Monotonic milliseconds.
    explicit IceAgent(Clock clock = {});

    // Owner must poll on its event loop, including while the network is idle.
    bool StartLiveness(uint64_t timeout_ms);
    // Terminal shutdown; construct a new agent to reopen after Close.
    void Close();
    // Compatibility alias for Close, not a temporary timer pause.
    void StopLiveness();
    bool CheckLiveness();

    void SetLocalCredentials(std::string ufrag, std::string pwd);
    void SetRemoteCredentials(std::string ufrag, std::string pwd);
    void SetRole(IceContext::Role role);
    void SetTieBreaker(uint64_t tie_breaker);
    void SetOnSelectedPeer(SelectedPeerCallback cb);

    const IceContext& Context() const { return ctx_; }

    HandleResult HandleDatagram(const network::SocketAddr& src, const uint8_t* data, size_t len, std::vector<uint8_t>& response);

    bool HasSelectedPeer() const { return CurrentState() == State::Completed; }
    const network::SocketAddr& SelectedPeer() const { return selected_peer_; }

private:
    enum class Event { Start, Check, Nominate, CredentialsChanged, Timeout, Close };
    using Lifecycle = utils::StateController<State, Event, IceAgent>;
    bool Transition(Event event);
    bool HandleBindingRequest(const network::SocketAddr& src, const protocol::StunMessageInfo& msg, std::vector<uint8_t>& response, HandleResult& result);

    bool BuildError(const protocol::StunMessageInfo& msg, uint16_t code, const std::string& reason, std::vector<uint8_t>& response,
                    const std::vector<uint16_t>& unknown = {});

    bool BuildSuccess(const network::SocketAddr& src, const protocol::StunMessageInfo& msg, std::vector<uint8_t>& response);

    bool ValidateUsername(const protocol::StunMessageInfo& msg, std::string& remote_ufrag);

    bool HasRoleConflict(const protocol::StunMessageInfo& msg) const;
    static bool SocketAddrToIpEndpoint(const network::SocketAddr& src, protocol::IpEndpoint& out);

private:
    IceContext ctx_;
    Clock clock_;
    uint64_t timeout_ms_{0};
    uint64_t last_check_ms_{0};
    network::SocketAddr selected_peer_{};
    SelectedPeerCallback on_selected_peer_;
    Lifecycle lifecycle_;
};

} // namespace ice

#endif /* _ICE_AGENT_H_ */
