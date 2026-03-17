#include "IceCandidate.h"
#include <sstream>
#include <string>
namespace ice
{

std::string IceCandidate::ToString() const
{
    std::ostringstream oss;
    return oss.str();
}

std::string IceCandidate::ToSdpCandidateLine() const
{
    std::ostringstream oss;

    oss << "candidate:" << foundation_ << " "
        << int(component_) << " "
        << (transport_ == TransportType::Udp ? "UDP" : "TCP") << " "
        << priority_ << " "
        << addr_.IpToString() << " "
        << addr_.Port()
        << " typ ";

    switch (type_)
    {
    case CandidateType::Host:  oss << "host"; break;
    case CandidateType::Srflx: oss << "srflx"; break;
    case CandidateType::Prflx: oss << "prflx"; break;
    case CandidateType::Relay: oss << "relay"; break;
    }

    if (rel_addr_)
    {
        oss << " raddr " << rel_addr_->IpToString()
            << " rport " << rel_addr_->Port();
    }

    return oss.str();
}

uint32_t IceCandidate::TypePreference(CandidateType t)
{
    switch (t)
    {
    case CandidateType::Host:  return 126;
    case CandidateType::Prflx: return 110;
    case CandidateType::Srflx: return 100;
    case CandidateType::Relay: return 0;
    }
    return 0;
}

static inline const char* SToString(IceCandidatePairState state)
{
    switch (state)
    {
    case IceCandidatePairState::Frozen:     return "frozen";
    case IceCandidatePairState::Waiting:    return "waiting";
    case IceCandidatePairState::InProgress: return "in-progress";
    case IceCandidatePairState::Succeeded:  return "succeeded";
    case IceCandidatePairState::Failed:     return "failed";
    default:                                return "unknown";
    }
}

uint32_t IceCandidate::ComputePriority(CandidateType type, uint16_t local_pref, uint8_t component)
{
    const uint32_t type_pref = TypePreference(type);
    return (type_pref << 24) | (uint32_t(local_pref) << 8) | (uint32_t(256 - component));
}

bool IceCandidatePair::Equal(const IceCandidatePair& other) const
{
    return CandidateEqual(local_, other.local_) &&
               CandidateEqual(remote_, other.remote_);
}

std::string IceCandidatePair::ToString() const
{
    std::ostringstream oss;
        oss << "prio " << Priority()
            << " (local, prio " << local_.Priority() << ") "
            << local_.ToString()
            << " <-> "
            << remote_.ToString()
            << " (remote, prio " << remote_.Priority() << ")"
            << ", state: " << SToString(State())
            << ", nominated: " << (Nominated() ? "true" : "false")
            << ", nominateOnBindingSuccess: "
            << (NominateOnBindingSuccess() ? "true" : "false");
        return oss.str();
}

uint64_t IceCandidatePair::Priority() const
{
    uint32_t g = 0;
    uint32_t d = 0;

    if (ice_role_controlling_)
    {
        g = local_.Priority();
        d = remote_.Priority();
    }
    else
    {
        g = remote_.Priority();
        d = local_.Priority();
    }

    const uint64_t min_v = static_cast<uint64_t>(std::min(g, d));
    const uint64_t max_v = static_cast<uint64_t>(std::max(g, d));
    const uint64_t cmp_v = (g > d) ? 1ULL : 0ULL;

    return (1ULL << 32) * min_v + 2ULL * max_v + cmp_v;
}

}