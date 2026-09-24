#include "IceAgent.h"

#include "logger.h"

#include <algorithm>
#include <cstring>
#include <utility>
#include <chrono>

namespace ice
{

namespace
{
constexpr size_t kMaxStunPacketSize = 1500;
constexpr uint16_t kBadRequest = 400;
constexpr uint16_t kUnauthorized = 401;
constexpr uint16_t kRoleConflict = 487;
} // namespace

IceAgent::IceAgent(Clock clock) : clock_(std::move(clock)),
    lifecycle_(*this, State::New, {
        Lifecycle::On(State::New, Event::Start, State::Checking),
        Lifecycle::On(State::Checking, Event::Start, State::Checking),
        Lifecycle::On(State::Completed, Event::Start, State::Checking),
        Lifecycle::On(State::Failed, Event::Start, State::Checking),
        Lifecycle::On(State::New, Event::Check, State::Checking),
        Lifecycle::Stay(State::Checking, Event::Check),
        Lifecycle::Stay(State::Completed, Event::Check),
        Lifecycle::On(State::Checking, Event::Nominate, State::Completed),
        Lifecycle::Stay(State::Completed, Event::Nominate),
        Lifecycle::Stay(State::New, Event::CredentialsChanged),
        Lifecycle::Stay(State::Checking, Event::CredentialsChanged),
        Lifecycle::On(State::Completed, Event::CredentialsChanged, State::Checking),
        Lifecycle::On(State::Checking, Event::Timeout, State::Failed),
        Lifecycle::On(State::Completed, Event::Timeout, State::Failed),
        Lifecycle::On(State::New, Event::Close, State::Closed),
        Lifecycle::On(State::Checking, Event::Close, State::Closed),
        Lifecycle::On(State::Completed, Event::Close, State::Closed),
        Lifecycle::On(State::Failed, Event::Close, State::Closed),
        Lifecycle::Stay(State::Closed, Event::Close)
    })
{
    if (!clock_) clock_ = [] {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    };
    ctx_.SetRole(IceContext::Role::Controlled);
}

bool IceAgent::Transition(Event event)
{
    const auto result = lifecycle_.Dispatch(event);
    if (!result.Accepted()) return false;
    if (event != Event::Check && event != Event::Nominate) selected_peer_ = {};
    return true;
}

bool IceAgent::StartLiveness(uint64_t timeout_ms)
{
    if (timeout_ms == 0 || !Transition(Event::Start)) return false;
    timeout_ms_ = timeout_ms;
    last_check_ms_ = clock_();
    return true;
}

void IceAgent::StopLiveness()
{
    Close();
}

void IceAgent::Close()
{
    timeout_ms_ = 0;
    Transition(Event::Close);
}

bool IceAgent::CheckLiveness()
{
    if (CurrentState() == State::Failed || CurrentState() == State::Closed) return false;
    if (timeout_ms_ == 0) return true;
    const auto now = clock_();
    if (now >= last_check_ms_ && now - last_check_ms_ >= timeout_ms_)
    {
        Transition(Event::Timeout);
        return false;
    }
    return true;
}

void IceAgent::SetLocalCredentials(std::string ufrag, std::string pwd)
{
    if (ufrag != ctx_.LocalUfrag() || pwd != ctx_.LocalPwd())
    {
        Transition(Event::CredentialsChanged);
    }
    ctx_.SetLocalCredentials(std::move(ufrag), std::move(pwd));
}

void IceAgent::SetRemoteCredentials(std::string ufrag, std::string pwd)
{
    if (ufrag != ctx_.RemoteUfrag() || pwd != ctx_.RemotePwd())
    {
        Transition(Event::CredentialsChanged);
    }
    ctx_.SetRemoteCredentials(std::move(ufrag), std::move(pwd));
}

void IceAgent::SetRole(IceContext::Role role)
{
    ctx_.SetRole(role);
}

void IceAgent::SetTieBreaker(uint64_t tie_breaker)
{
    ctx_.SetTieBreaker(tie_breaker);
}

void IceAgent::SetOnSelectedPeer(SelectedPeerCallback cb)
{
    on_selected_peer_ = std::move(cb);
}

IceAgent::HandleResult IceAgent::HandleDatagram(const network::SocketAddr& src,
                                                const uint8_t* data,
                                                size_t len,
                                                std::vector<uint8_t>& response)
{
    response.clear();
    if (!CheckLiveness()) return HandleResult::Ignored;

    protocol::StunMessageInfo msg;
    if (!protocol::StunCodec::Parse(data, len, msg))
    {
        return HandleResult::NotStun;
    }

    if (msg.raw_len != len || !msg.IsBindingRequest())
    {
        return HandleResult::Ignored;
    }

    HandleResult result = HandleResult::Ignored;
    if (!HandleBindingRequest(src, msg, response, result))
    {
        return result;
    }
    return result;
}

bool IceAgent::HandleBindingRequest(const network::SocketAddr& src,
                                    const protocol::StunMessageInfo& msg,
                                    std::vector<uint8_t>& response,
                                    HandleResult& result)
{
    if (!ctx_.HasLocalCredentials())
    {
        LOG_ERROR("[ICE] local credentials are not configured");
        result = HandleResult::Ignored;
        return false;
    }

    const bool has_fingerprint =
        msg.HasAttr(static_cast<uint16_t>(protocol::AttrType::FINGERPRINT));
    if (!has_fingerprint || !protocol::StunCodec::VerifyFingerprint(msg))
    {
        // Discard corrupted datagrams instead of reflecting an error response.
        result = HandleResult::Ignored;
        return false;
    }

    std::string remote_ufrag;
    if (!msg.HasAttr(static_cast<uint16_t>(protocol::AttrType::USERNAME)) ||
        !msg.HasAttr(static_cast<uint16_t>(protocol::AttrType::MESSAGE_INTEGRITY)))
    {
        BuildError(msg, kBadRequest, "Missing credentials", response);
        result = HandleResult::ErrorResponse;
        return false;
    }
    if (!ValidateUsername(msg, remote_ufrag))
    {
        BuildError(msg, kUnauthorized, "Bad USERNAME", response);
        result = HandleResult::ErrorResponse;
        return false;
    }

    if (!protocol::StunCodec::VerifyMessageIntegrity(msg, ctx_.InboundIntegrityKey()))
    {
        BuildError(msg, kUnauthorized, "Bad MESSAGE-INTEGRITY", response);
        result = HandleResult::ErrorResponse;
        return false;
    }

    uint32_t priority = 0;
    uint64_t controlling = 0, controlled = 0;
    const bool has_controlling = protocol::StunCodec::DecodeIceControlling(msg, controlling);
    const bool has_controlled = protocol::StunCodec::DecodeIceControlled(msg, controlled);
    const auto* nomination = msg.FindAttr(static_cast<uint16_t>(protocol::AttrType::USE_CANDIDATE));
    const bool both_roles = msg.HasAttr(static_cast<uint16_t>(protocol::AttrType::ICE_CONTROLLING)) &&
                            msg.HasAttr(static_cast<uint16_t>(protocol::AttrType::ICE_CONTROLLED));
    if (!protocol::StunCodec::DecodePriority(msg, priority) || priority == 0 || both_roles ||
        has_controlling == has_controlled ||
        (nomination && (nomination->len != 0 || !has_controlling)))
    {
        BuildError(msg, kBadRequest, "Bad ICE attributes", response);
        result = HandleResult::ErrorResponse;
        return false;
    }

    std::vector<uint16_t> unknown;
    for (const auto& attr : msg.attrs)
    {
        const auto type = static_cast<protocol::AttrType>(attr.type);
        if (type == protocol::AttrType::MESSAGE_INTEGRITY) break;
        if (attr.type < 0x8000 && type != protocol::AttrType::USERNAME &&
            type != protocol::AttrType::PRIORITY && type != protocol::AttrType::USE_CANDIDATE &&
            std::find(unknown.begin(), unknown.end(), attr.type) == unknown.end())
            unknown.push_back(attr.type);
    }
    if (!unknown.empty())
    {
        BuildError(msg, 420, "Unknown Attribute", response, unknown);
        result = HandleResult::ErrorResponse;
        return false;
    }

    if (HasRoleConflict(msg))
    {
        BuildError(msg, kRoleConflict, "Role Conflict", response);
        result = HandleResult::ErrorResponse;
        return false;
    }

    if (!BuildSuccess(src, msg, response))
    {
        result = HandleResult::Ignored;
        return false;
    }

    // Never learn credentials or change the selected tuple before authentication.
    Transition(Event::Check);
    if (ctx_.RemoteUfrag().empty())
        ctx_.SetRemoteCredentials(remote_ufrag, ctx_.RemotePwd());

    if (!ctx_.IsControlling() && protocol::StunCodec::HasUseCandidate(msg) &&
        (!HasSelectedPeer() || !(selected_peer_ == src)))
    {
        selected_peer_ = src;
        Transition(Event::Nominate);
        if (on_selected_peer_)
        {
            on_selected_peer_(selected_peer_);
        }
        LOG_INFO("[ICE] selected peer by USE-CANDIDATE, peer=", src.ToString());
    }

    // Only valid checks from the selected tuple refresh the watchdog. Checks
    // from another address and media traffic cannot keep this path alive.
    if (timeout_ms_ != 0 && HasSelectedPeer() && selected_peer_ == src)
        last_check_ms_ = clock_();

    result = HandleResult::SuccessResponse;
    return true;
}

bool IceAgent::ValidateUsername(const protocol::StunMessageInfo& msg,
                                std::string& remote_ufrag)
{
    remote_ufrag.clear();

    std::string_view username;
    if (!protocol::StunCodec::DecodeUsername(msg, username))
    {
        return false;
    }

    const auto pos = username.find(':');
    if (pos == std::string_view::npos)
    {
        return false;
    }

    const std::string_view local = username.substr(0, pos);
    const std::string_view remote = username.substr(pos + 1);
    if (local != ctx_.LocalUfrag() || remote.empty() || remote.find(':') != std::string_view::npos)
    {
        return false;
    }

    if (!ctx_.RemoteUfrag().empty() && remote != ctx_.RemoteUfrag())
    {
        return false;
    }

    remote_ufrag.assign(remote.data(), remote.size());
    return true;
}

bool IceAgent::HasRoleConflict(const protocol::StunMessageInfo& msg) const
{
    uint64_t tie_breaker = 0;
    if (ctx_.IsControlling())
    {
        return protocol::StunCodec::DecodeIceControlling(msg, tie_breaker);
    }

    return protocol::StunCodec::DecodeIceControlled(msg, tie_breaker);
}

bool IceAgent::BuildError(const protocol::StunMessageInfo& msg,
                          uint16_t code,
                          const std::string& reason,
                          std::vector<uint8_t>& response,
                          const std::vector<uint16_t>& unknown)
{
    uint8_t buf[kMaxStunPacketSize] = {0};
    size_t out_len = 0;
    protocol::StunErrorCode err;
    err.code = code;
    err.reason = reason;
    err.unknown_attributes = unknown;

    if (!protocol::StunCodec::BuildBindingError(msg,
                                                err,
                                                (code == kBadRequest || code == kUnauthorized)
                                                    ? std::string_view{} : ctx_.InboundIntegrityKey(),
                                                buf,
                                                sizeof(buf),
                                                out_len))
    {
        return false;
    }

    response.assign(buf, buf + out_len);
    return true;
}

bool IceAgent::BuildSuccess(const network::SocketAddr& src,
                            const protocol::StunMessageInfo& msg,
                            std::vector<uint8_t>& response)
{
    protocol::IpEndpoint mapped;
    if (!SocketAddrToIpEndpoint(src, mapped))
    {
        return false;
    }

    uint8_t buf[kMaxStunPacketSize] = {0};
    size_t out_len = 0;
    protocol::IceSuccessParams params;
    params.req = &msg;
    params.mapped_addr = mapped;
    params.password = ctx_.InboundIntegrityKey();
    params.add_fingerprint = true;

    if (!protocol::StunCodec::BuildIceBindingSuccess(params, buf, sizeof(buf), out_len))
    {
        return false;
    }

    response.assign(buf, buf + out_len);
    return true;
}

bool IceAgent::SocketAddrToIpEndpoint(const network::SocketAddr& src,
                                      protocol::IpEndpoint& out)
{
    out = {};
    out.port = src.Port();

    if (src.IsV4())
    {
        out.family = protocol::IpFamily::IPv4;
        const std::string ip = src.IPv4Bytes();
        if (ip.size() != 4)
        {
            return false;
        }
        std::memcpy(out.ip.data(), ip.data(), 4);
        return true;
    }

    if (src.IsV6())
    {
        out.family = protocol::IpFamily::IPv6;
        const std::string ip = src.IPv6Bytes();
        if (ip.size() != 16)
        {
            return false;
        }
        std::memcpy(out.ip.data(), ip.data(), 16);
        return true;
    }

    return false;
}

} // namespace ice
