#ifndef _ICECANDIDATESTORE_H_
#define _ICECANDIDATESTORE_H_

#include <vector>
#include <cstdint>
#include "IceCandidate.h"
namespace ice
{

class IceCandidateStore 
{
public:
    void AddLocal(const IceCandidate& c);
    void AddRemote(const IceCandidate& c);

    std::vector<IceCandidate> GetLocals(uint8_t component) const;
    std::vector<IceCandidate> GetRemotes(uint8_t component) const;

    // for SDP
    std::vector<std::string> LocalSdpLines() const;

private:
    static bool SameCandidate(const IceCandidate& a, const IceCandidate& b);

private:
    std::vector<IceCandidate> locals_;
    std::vector<IceCandidate> remotes_;
};
}

#endif /* _ICECANDIDATESTORE_H_ */