#include "IceAgent.h"

#include "logger.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace ice
{

namespace
{
constexpr size_t kMaxStunPacketSize = 1500;
constexpr uint16_t kBadRequest = 400;
constexpr uint16_t kUnauthorized = 401;
constexpr uint16_t kRoleConflict = 487;
} // namespace

IceAgent::IceAgent()
{
    ctx_.SetRole(IceContext::Role::Controlled);
}

void IceAgent::SetLocalCredentials(std::string ufrag, std::string pwd)
{
    ctx_.SetLocalCredentials(std::move(ufrag), std::move(pwd));
}

void IceAgent::SetRemoteCredentials(std::string ufrag, std::string pwd)
{
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

    protocol::StunMessageInfo msg;
    if (!protocol::StunCodec::Parse(data, len, msg))
    {
        return HandleResult::NotStun;
    }

    if (!msg.IsBindingRequest())
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
    if (has_fingerprint && !protocol::StunCodec::VerifyFingerprint(msg))
    {
        BuildError(msg, kBadRequest, "Bad FINGERPRINT", response);
        result = HandleResult::ErrorResponse;
        return false;
    }

    std::string remote_ufrag;
    if (!ValidateUsername(msg, remote_ufrag))
    {
        BuildError(msg, kUnauthorized, "Bad USERNAME", response);
        result = HandleResult::ErrorResponse;
        return false;
    }

    if (ctx_.RemoteUfrag().empty() && !remote_ufrag.empty())
    {
        ctx_.SetRemoteCredentials(remote_ufrag, ctx_.RemotePwd());
    }

    if (!protocol::StunCodec::VerifyMessageIntegrity(msg, ctx_.InboundIntegrityKey()))
    {
        BuildError(msg, kUnauthorized, "Bad MESSAGE-INTEGRITY", response);
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

    if (protocol::StunCodec::HasUseCandidate(msg))
    {
        selected_peer_ = src;
        has_selected_peer_ = true;
        if (on_selected_peer_)
        {
            on_selected_peer_(selected_peer_);
        }
        LOG_INFO("[ICE] selected peer by USE-CANDIDATE, peer=", src.ToString());
    }

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
    if (local != ctx_.LocalUfrag())
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
                          std::vector<uint8_t>& response)
{
    uint8_t buf[kMaxStunPacketSize] = {0};
    size_t out_len = 0;
    protocol::StunErrorCode err;
    err.code = code;
    err.reason = reason;

    if (!protocol::StunCodec::BuildBindingError(msg,
                                                err,
                                                ctx_.InboundIntegrityKey(),
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
