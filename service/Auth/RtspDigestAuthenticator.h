#pragma once

#include "Authenticator.h"
#include "RtspAuthOptions.h"
#include <functional>
#include <map>
#include <memory>

namespace service::auth
{
class IRtspCredentialProvider
{
public:
    virtual ~IRtspCredentialProvider() = default;
    virtual std::optional<RtspCredential> Find(std::string_view username) const = 0;
};

// 启动时加载的不可变凭据快照，可被多个连接共享，不访问账号数据库。
class StaticRtspCredentialProvider final : public IRtspCredentialProvider
{
public:
    explicit StaticRtspCredentialProvider(const RtspAuthOptions& options);
    std::optional<RtspCredential> Find(std::string_view username) const override;
private:
    std::map<std::string, RtspCredential, std::less<>> users_;
};

// 每个 RTSP 连接独享，由连接 owner 串行调用；nonce 不跨连接复用。
class RtspDigestAuthenticator final : public IAuthenticator
{
public:
    using Clock = std::function<int64_t()>;
    RtspDigestAuthenticator(std::shared_ptr<const IRtspCredentialProvider> credentials, std::string realm,
                            uint32_t nonce_ttl_seconds, bool allow_legacy, Clock clock = {});
    AuthResult Authenticate(std::string_view, Audience) const override;
    AuthResult Authenticate(const AuthRequest& request, Audience audience) const override;
    std::string Challenge(bool stale = false) const;
private:
    const std::shared_ptr<const IRtspCredentialProvider> credentials_;
    const std::string realm_;
    const uint32_t ttl_;
    const bool allow_legacy_;
    const Clock clock_;
    mutable std::string nonce_;
    mutable int64_t issued_at_ = 0;
    mutable uint32_t last_nc_ = 0;
    mutable bool legacy_used_ = false;
};
}
