#include "IceChecklist.h"
#include <algorithm>
#include <unordered_set>

namespace ice 
{

uint64_t IceChecklist::ComputePairPriority(uint32_t g, uint32_t d)
{
    uint32_t minv = std::min(g, d);
    uint32_t maxv = std::max(g, d);
    return ( (uint64_t)minv << 32 ) + ( (uint64_t)maxv << 1 ) + (g > d ? 1 : 0);
}

void IceChecklist::BuildPairs(const std::vector<IceCandidate>& locals,
                              const std::vector<IceCandidate>& remotes)
{
    pairs_.clear();
    pairs_.reserve(locals.size() * remotes.size());
    struct Key {
        uint8_t comp;
        TransportType tp;
        std::string la;
        std::string ra;

        bool operator==(const Key& o) const 
        {
            return comp == o.comp && tp == o.tp && la == o.la && ra == o.ra;
        }
    };
    struct KeyHash {
        size_t operator()(const Key& k) const noexcept {
            std::hash<std::string> hs;
            size_t h = 1469598103934665603ull;
            auto mix = [&](size_t x) { h ^= x + 0x9e3779b97f4a7c15ull + (h<<6) + (h>>2); };
            mix((size_t)k.comp);
            mix((size_t)k.tp);
            mix(hs(k.la));
            mix(hs(k.ra));
            return h;
        }
    };

    std::unordered_set<Key, KeyHash> seen;
    seen.reserve(locals.size() * remotes.size());

    for (const auto& lc : locals) 
    {
        for (const auto& rc : remotes) 
        {
            if (lc.Component() != rc.Component()) continue;
            if (lc.Transport() != rc.Transport()) continue;


            const std::string la = lc.Addr().ToString(); 
            const std::string ra = rc.Addr().ToString();

            Key key{lc.Component(), lc.Transport(), la, ra};
            if (!seen.insert(key).second) continue;

    
            uint32_t g = 0, d = 0;
            if (role_ == IceRole::Controlling) 
            {
                g = lc.Priority();
                d = rc.Priority();
            }
            else 
            {
                g = rc.Priority();
                d = lc.Priority();
            }
            const uint64_t pp = ComputePairPriority(g, d);

            IceCandidatePair p;
            p.local = lc;
            p.remote = rc;
            p.priority = pp;
            pairs_.push_back(std::move(p));
        }
    }
}

void IceChecklist::SortPairs()
{
    std::stable_sort(pairs_.begin(), pairs_.end(),
        [](const IceCandidatePair& a, const IceCandidatePair& b) {

            if (a.priority != b.priority)
                return a.priority > b.priority;

            
            const bool a_relay = a.local.IsRelay() || a.remote.IsRelay();
            const bool b_relay = b.local.IsRelay() || b.remote.IsRelay();
            if (a_relay != b_relay)
                return !a_relay; 

            auto rank = [](CandidateType t) -> int 
            {
                switch (t) 
                {
                case CandidateType::Host:  return 3;
                case CandidateType::Srflx: return 2;
                case CandidateType::Prflx: return 2;
                case CandidateType::Relay: return 1;
                default: return 0;
                }
            };
            const int a_lr = rank(a.local.Type());
            const int b_lr = rank(b.local.Type());
            if (a_lr != b_lr) return a_lr > b_lr;

            const int a_rr = rank(a.remote.Type());
            const int b_rr = rank(b.remote.Type());
            if (a_rr != b_rr) return a_rr > b_rr;

            const std::string a_key =
                a.local.Addr().ToString() + "->" + a.remote.Addr().ToString();
            const std::string b_key =
                b.local.Addr().ToString() + "->" + b.remote.Addr().ToString();
            return a_key < b_key;
        });
}

void IceChecklist::PruneRedundantPairs()
{
    SortPairs();
    struct Key {
        uint8_t comp;
        TransportType tp;
        std::string local_base;
        std::string remote_addr;

        bool operator==(const Key& o) const {
            return comp == o.comp && tp == o.tp &&
                   local_base == o.local_base && remote_addr == o.remote_addr;
        }
    };
    struct KeyHash {
        size_t operator()(const Key& k) const noexcept {
            std::hash<std::string> hs;
            size_t h = 0;
            auto mix = [&](size_t x) { h ^= x + 0x9e3779b97f4a7c15ull + (h<<6) + (h>>2); };
            mix((size_t)k.comp);
            mix((size_t)k.tp);
            mix(hs(k.local_base));
            mix(hs(k.remote_addr));
            return h;
        }
    };

    std::unordered_set<Key, KeyHash> seen;
    seen.reserve(pairs_.size());

    auto local_base_str = [](const IceCandidate& c) -> std::string {
    
        if (c.IsReflexive()) return c.BaseAddr().ToString();
        return c.Addr().ToString();
    };

    std::vector<IceCandidatePair> kept;
    kept.reserve(pairs_.size());

    for (const auto& p : pairs_) {
        Key k;
        k.comp = p.local.Component();
        k.tp = p.local.Transport();
        k.local_base = local_base_str(p.local);
        k.remote_addr = p.remote.Addr().ToString();

        if (seen.insert(k).second) 
        {
            kept.push_back(p);   
        } 
        else 
        {
          
        }
    }

    pairs_.swap(kept);
}

IceCandidatePair* IceChecklist::Selected()
{
    for (auto& p : pairs_) 
    {
        if (p.nominated && p.state == PairState::Succeeded) 
        {
            return &p;
        }
    }

    for (auto& p : pairs_) 
    {
        if (p.state == PairState::Succeeded) 
        {
            return &p;
        }
    }
    return nullptr;
}

const IceCandidatePair* IceChecklist::Selected() const
{
    for (const auto& p : pairs_) 
    {
        if (p.nominated && p.state == PairState::Succeeded) 
        {
            return &p;
        }
    }
    for (const auto& p : pairs_) 
    {
        if (p.state == PairState::Succeeded) 
        {
            return &p;
        }
    }
    return nullptr;
}

}