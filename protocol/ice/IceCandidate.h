#ifndef _ICE_PAIR_H_
#define _ICE_PAIR_H_

#include "Endpoint.h"
#include <cstdint>
#include <string>
#include <optional>



// IceCandidate represents a single ICE candidate as defined by ICE (RFC 8445 / RFC 5245).
//
// An ICE agent gathers multiple candidates (Host, Srflx, Relay; Prflx may be discovered during checks),
// exchanges them with the peer via signaling (e.g., SDP "a=candidate" lines), and then forms
// candidate pairs (local candidate + remote candidate). Each pair is validated via STUN connectivity
// checks, and the best valid/nominated pair becomes the selected path for media/data transport.
//
// Key fields:
// - component_: Component ID (commonly 1=RTP, 2=RTCP or any app-defined components).
// - transport_: Transport protocol (typically UDP).
// - type_: Candidate type: host / srflx (server-reflexive) / prflx (peer-reflexive) / relay.
// - priority_: Candidate priority used for sorting and pair prioritization.
// - foundation_: A grouping key used for checklist formation, unfreezing, and pruning redundant pairs.
// - addr_: The candidate transport address advertised to the peer (<ip, port> in SDP).
// - base_addr_: The base address for reflexive candidates (usually the related host candidate).
// - rel_addr_: Related address (raddr/rport in SDP), typically present for srflx/relay candidates.
// - network_id_/network_cost_: Optional local network metadata for multi-interface preference.

namespace ice
{

enum class PairState { Frozen, Waiting, InProgress, Succeeded, Failed };

enum class CandidateType
{
   Host,
   Srflx,
   Prflx,
   Relay
};

enum class TransportType 
{
    Udp,
    Tcp
};




class IceCandidate
{
public:
   IceCandidate() = default;

   static IceCandidate Host(uint8_t component,
                             const net::Endpoint& addr,
                             TransportType tp = TransportType::Udp,
                             uint16_t local_pref = 65535,
                             uint16_t network_id = 0,
                             uint16_t network_cost = 0);

   static IceCandidate Srflx(uint8_t component,
                              const net::Endpoint& srflx_addr,
                              const net::Endpoint& base_addr,
                              std::optional<net::Endpoint> rel_addr = std::nullopt,
                              TransportType tp = TransportType::Udp,
                              uint16_t local_pref = 65535,
                              uint16_t network_id = 0,
                              uint16_t network_cost = 0);

   static IceCandidate Relay(uint8_t component,
                              const net::Endpoint& relay_addr,
                              std::optional<net::Endpoint> rel_addr = std::nullopt,
                              TransportType tp = TransportType::Udp,
                              uint16_t local_pref = 65535,
                              uint16_t network_id = 0,
                              uint16_t network_cost = 0);


      const std::string& Foundation() const { return foundation_; }
      uint8_t Component() const { return component_; }
      TransportType Transport() const { return transport_; }
      CandidateType Type() const { return type_; }
      uint32_t Priority() const { return priority_; }

      const net::Endpoint& Addr() const { return addr_; }
      const net::Endpoint& BaseAddr() const { return base_addr_; }
      const std::optional<net::Endpoint>& RelAddr() const { return rel_addr_; }

      uint16_t NetworkId() const { return network_id_; }
      uint16_t NetworkCost() const { return network_cost_; }

      bool IsReflexive() const { return type_ == CandidateType::Srflx || type_ == CandidateType::Prflx; }
      bool IsRelay() const { return type_ == CandidateType::Relay; }
      bool IsHost() const { return type_ == CandidateType::Host; }

      std::string ToString() const;
      std::string ToSdpCandidateLine() const;

private:
      static uint32_t TypePreference(CandidateType t);
      static uint32_t ComputePriority(CandidateType type, uint16_t local_pref, uint8_t component);
      

private:
   std::string foundation_;
    uint8_t component_{1};
    TransportType transport_{TransportType::Udp};
    CandidateType type_{CandidateType::Host};
    uint32_t priority_{0};

    net::Endpoint addr_{};
    net::Endpoint base_addr_{};
    std::optional<net::Endpoint> rel_addr_;

    uint16_t network_id_{0};
    uint16_t network_cost_{0};

};


typedef struct IceCandidatePair 
{
    IceCandidate local;
    IceCandidate remote;

    uint64_t priority = 0;             /* pair priority (RFC) */
    PairState state = PairState::Frozen;

    bool valid = false;                /* connectivity check succeeded */
    bool nominated = false;            /* USE-CANDIDATE accepted */


    std::array<uint8_t, 12> txid{};    /* STUN transaction id (96-bit) */
    int retransmit = 0;
    uint64_t next_rto_ms = 0;
    uint64_t last_send_ms = 0;
    PairState State;

    std::string ToString() const; 
}IceCandidatePair;


} // namespace ice



#endif /* _ICE_H_ */