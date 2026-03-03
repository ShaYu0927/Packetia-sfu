#ifndef _ICE_PAIR_H_
#define _ICE_PAIR_H_

#include "Endpoint.h"
#include <cstdint>
#include <string>
#include <optional>


/*
   RFC 8445
*/
namespace ice
{

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



} // namespace ice



#endif /* _ICE_H_ */