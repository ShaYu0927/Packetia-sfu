#ifndef _ICECHECKLIST_H_
#define _ICECHECKLIST_H_

#include "IceCandidate.h"
#include <vector>

namespace ice 
{
class IceChecklist 
{
public:
    enum class IceRole { Controlling, Controlled };
    IceChecklist() = default;

    const std::vector<IceCandidatePairEntry>& pairs() const { return pairs_; }
    std::vector<IceCandidatePairEntry>& pairs() { return pairs_; }

    void BuildPairs(const std::vector<IceCandidate>& locals,
                    const std::vector<IceCandidate>& remotes);

    /* Sort pairs by pair priority (and potentially other heuristics). */
    void SortPairs();

    /* Prune redundant pairs (RFC 8445-ish) */
    void PruneRedundantPairs();

    /* Update checklist state based on pair outcomes. */
    void UpdateChecklistState();

    IceCandidatePairEntry* Selected();
    const IceCandidatePairEntry* Selected() const;

private:
    static uint64_t ComputePairPriority(uint32_t g, uint32_t d);

private:
    std::vector<IceCandidatePairEntry> pairs_;
    IceRole role_{IceRole::Controlling};
};

}

#endif /* _ICECHECKLIST_H_ */
