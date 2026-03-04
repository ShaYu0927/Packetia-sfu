#ifndef _ICECHECKLIST_H_
#define _ICECHECKLIST_H_

#include "IceCandidate.h"
#include <vector>

namespace ice 
{
class IceChecklist 
{
public:
    IceChecklist() = default;

    const std::vector<IceCandidatePair>& pairs() const { return pairs_; }
    std::vector<IceCandidatePair>& pairs() { return pairs_; }

    void BuildPairs(const std::vector<IceCandidate>& locals,
                    const std::vector<IceCandidate>& remotes);

    // Sort pairs by pair priority (and potentially other heuristics).
    void SortPairs();

    // Prune redundant pairs (RFC 8445-ish)
    // Redundant if local bases are same and remote candidates identical, etc.
    void PruneRedundantPairs();

    // Update checklist state based on pair outcomes.
    void UpdateChecklistState();

    // Find selected pair (if any)
    IceCandidatePair* Selected();
    const IceCandidatePair* Selected() const;

private:
    static uint64_t ComputePairPriority(uint32_t g, uint32_t d);

private:
    std::vector<IceCandidatePair> pairs_;
};

}

#endif /* _ICECHECKLIST_H_ */
