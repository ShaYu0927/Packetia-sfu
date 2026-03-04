#include "IceCandidateStore.h"
#include <algorithm>

namespace ice 
{
void IceCandidateStore::AddLocal(const IceCandidate& c)
{
    auto it = std::find_if(locals_.begin(), locals_.end(),
                           [&](const IceCandidate& e) { return SameCandidate(e, c); });
    if (it != locals_.end()) return;
    locals_.push_back(c);
}

void IceCandidateStore::AddRemote(const IceCandidate& c)
{
    auto it = std::find_if(remotes_.begin(), remotes_.end(),
                           [&](const IceCandidate& e) { return SameCandidate(e, c); });
    if (it != remotes_.end()) return;
    remotes_.push_back(c);
}

std::vector<IceCandidate> IceCandidateStore::GetLocals(uint8_t component) const
{
    std::vector<IceCandidate> out;
    out.reserve(locals_.size());
    for (const auto& c : locals_) {
        if (c.Component() == component) out.push_back(c);
    }
    return out;
}

std::vector<IceCandidate> IceCandidateStore::GetRemotes(uint8_t component) const
{
    std::vector<IceCandidate> out;
    out.reserve(remotes_.size());
    for (const auto& c : remotes_) {
        if (c.Component() == component) out.push_back(c);
    }
    return out;
}

std::vector<std::string> IceCandidateStore::LocalSdpLines() const
{
    std::vector<std::string> out;
    out.reserve(locals_.size());
    for (const auto& c : locals_) {
        out.push_back(c.ToSdpCandidateLine());
    }
    return out;
}

bool IceCandidateStore::SameCandidate(const IceCandidate& a, const IceCandidate& b)
{
    if (a.Component() != b.Component()) return false;
    if (a.Transport() != b.Transport()) return false;
    if (a.Type() != b.Type()) return false;

    return a.Addr() == b.Addr();
}

}