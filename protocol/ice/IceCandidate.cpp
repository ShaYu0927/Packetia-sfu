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

uint32_t IceCandidate::ComputePriority(CandidateType type, uint16_t local_pref, uint8_t component)
{
    const uint32_t type_pref = TypePreference(type);
    return (type_pref << 24) | (uint32_t(local_pref) << 8) | (uint32_t(256 - component));
}

}