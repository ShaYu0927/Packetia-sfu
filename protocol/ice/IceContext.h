#ifndef _ICE_CFG_H_
#define _ICE_CFG_H_

#include <cstdint>
#include <string>

class IceContext
{
public:
    enum class Role 
    {
        Controlling,
        Controlled
    };

    IceContext() = default;

    void SetLocalCredentials(std::string ufrag, std::string pwd);
    void SetRemoteCredentials(std::string ufrag, std::string pwd);


    void SetRole(Role r) { role_ = r; }
    void SetTieBreaker(uint64_t v) { tie_breaker_ = v; }

    const std::string& LocalUfrag()  const { return local_ufrag_; }
    const std::string& LocalPwd()    const { return local_pwd_; }
    const std::string& RemoteUfrag() const { return remote_ufrag_; }
    const std::string& RemotePwd()   const { return remote_pwd_; }

    Role GetRole() const { return role_; }
    bool IsControlling() const { return role_ == Role::Controlling; }
    uint64_t TieBreaker() const { return tie_breaker_; }

    bool HasLocalCredentials() const { return !local_ufrag_.empty() && !local_pwd_.empty(); }
    bool HasRemoteCredentials() const { return !remote_ufrag_.empty() && !remote_pwd_.empty(); }
    bool ReadyForChecks() const { return HasLocalCredentials() && HasRemoteCredentials(); }

    const std::string& OutboundIntegrityKey() const { return remote_pwd_; }
    const std::string& InboundIntegrityKey()  const { return local_pwd_; }

    bool CanNominate() const { return IsControlling(); }

    std::string BuildStunUsername() const 
    {
        if (remote_ufrag_.empty() || local_ufrag_.empty()) return {};
        return remote_ufrag_ + ":" + local_ufrag_;
    }


private:
    std::string local_ufrag_;
    std::string local_pwd_;
    std::string remote_ufrag_;
    std::string remote_pwd_;

    Role role_{Role::Controlled};
    uint64_t tie_breaker_{0};

};

#endif /* _ICE_CFG_H_ */