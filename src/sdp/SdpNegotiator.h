#ifndef PACKETIA_SDP_NEGOTIATOR_H_
#define PACKETIA_SDP_NEGOTIATOR_H_

#include "SdpMode.h"

namespace sdp
{

enum class SdpNegotiationState
{
    Stable, HaveLocalOffer, HaveRemoteOffer
};

class SdpNegotiator
{
public:
    SdpNegotiator() = default;
    SdpNegotiator(const SdpNegotiator&) = delete;
    SdpNegotiator& operator=(const SdpNegotiator&) = delete;
    
    bool CreateOffer(const SdpSession& local, SdpSession& offer);

    bool ApplyOffer(const SdpSession& offer);

    bool CreateAnswer(const SdpSession& local, SdpSession& answer);

    bool ApplyAnswer(const SdpSession& answer);

    bool Commit() noexcept;

    void Rollback() noexcept;
    SdpNegotiationState State() const noexcept { return state_; }
    bool HasCurrent() const noexcept { return has_current_; }
    bool PendingReady() const noexcept { return pending_ready_; }
    const std::string& LastError() const noexcept { return last_error_; }

    const SdpSession& PendingLocal() const noexcept { return pending_local_; }
    const SdpSession& PendingRemote() const noexcept { return pending_remote_; }
    const SdpSession& CurrentLocal() const noexcept { return current_local_; }
    const SdpSession& CurrentRemote() const noexcept { return current_remote_; }

private:
    bool Reject(const std::string& error);
    bool SetLocalOrigin(SdpSession& local);
    SdpNegotiationState state_ = SdpNegotiationState::Stable;
    bool has_current_ = false;
    bool pending_ready_ = false;
    SdpSession current_local_, current_remote_;
    SdpSession pending_local_, pending_remote_, wire_offer_;
    std::string local_origin_id_;
    uint64_t next_origin_version_ = 0;
    bool origin_version_exhausted_ = false;
    std::string last_error_;
};

} // namespace sdp

#endif // PACKETIA_SDP_NEGOTIATOR_H_
