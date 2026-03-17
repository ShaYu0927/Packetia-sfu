#ifndef _ICE_PAIR_H_
#define _ICE_PAIR_H_

#include "Endpoint.h"
#include <atomic>
#include <chrono>
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

enum class IceCandidatePairState
{
    Frozen = 0,
    Waiting,
    InProgress,
    Succeeded,
    Failed
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

    const net::Endpoint& Address() const { return addr_; }
    const net::Endpoint& BaseAddress() const { return base_addr_; }
    const std::optional<net::Endpoint>& RelatedAddress() const { return rel_addr_; }

    void SetFoundation(const std::string& v) { foundation_ = v; }
    void SetComponent(uint8_t v) { component_ = v; }
    void SetTransport(TransportType v) { transport_ = v; }
    void SetType(CandidateType v) { type_ = v; }
    void SetPriority(uint32_t v) { priority_ = v; }

    void SetAddress(const net::Endpoint& v) { addr_ = v; }
    void SetBaseAddress(const net::Endpoint& v) { base_addr_ = v; }
    void SetRelatedAddress(const std::optional<net::Endpoint>& v) { rel_addr_ = v; }

    void SetNetworkId(uint16_t v) { network_id_ = v; }
    void SetNetworkCost(uint16_t v) { network_cost_ = v; }


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


typedef struct IceCandidatePairEntry 
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
}IceCandidatePairEntry;

class IceCandidatePair
{
public:
    using Clock = std::chrono::system_clock;
    using TimePoint = Clock::time_point;

public:
    IceCandidatePair() = default;



    IceCandidatePair(const IceCandidate& local,
                     const IceCandidate& remote,
                     bool controlling)
        : local_(local),
          remote_(remote),
          ice_role_controlling_(controlling),
          state_(IceCandidatePairState::Waiting)
    {
    }

    static IceCandidatePair Create(const IceCandidate& local,
                                   const IceCandidate& remote,
                                   bool controlling)
    {
        return IceCandidatePair(local, remote, controlling);
    }

public:
    const IceCandidate& Local() const { return local_; }
    const IceCandidate& Remote() const { return remote_; }

    void SetLocal(const IceCandidate& v) { local_ = v; }
    void SetRemote(const IceCandidate& v) { remote_ = v; }

    bool IsControlling() const { return ice_role_controlling_; }
    void SetControlling(bool v) { ice_role_controlling_ = v; }

    uint64_t Id() const { return id_.load(std::memory_order_relaxed); }
    void SetId(uint64_t v) { id_.store(v, std::memory_order_relaxed); }
    

    IceCandidatePairState State() const
    {
        return state_.load(std::memory_order_relaxed);
    }

    void SetState(IceCandidatePairState s)
    {
        state_.store(s, std::memory_order_relaxed);
    }

    bool Nominated() const
    {
        return nominated_.load(std::memory_order_relaxed);
    }

    void SetNominated(bool v)
    {
        nominated_.store(v, std::memory_order_relaxed);
    }

    bool NominateOnBindingSuccess() const
    {
        return nominate_on_binding_success_.load(std::memory_order_relaxed);
    }

    void SetNominateOnBindingSuccess(bool v)
    {
        nominate_on_binding_success_.store(v, std::memory_order_relaxed);
    }

    uint16_t BindingRequestCount() const
    {
        return binding_request_count_.load(std::memory_order_relaxed);
    }

    void IncBindingRequestCount()
    {
        binding_request_count_.fetch_add(1, std::memory_order_relaxed);
    }

    bool Equal(const IceCandidatePair& other) const;

    std::string ToString() const;

    uint64_t Priority() const;


public:
    uint64_t RequestsReceived() const
    {
        return requests_received_.load(std::memory_order_relaxed);
    }

    uint64_t RequestsSent() const
    {
        return requests_sent_.load(std::memory_order_relaxed);
    }

    uint64_t ResponsesReceived() const
    {
        return responses_received_.load(std::memory_order_relaxed);
    }

    uint64_t ResponsesSent() const
    {
        return responses_sent_.load(std::memory_order_relaxed);
    }

    void UpdateRequestSent()
    {
        requests_sent_.fetch_add(1, std::memory_order_relaxed);

        const int64_t now_ns = NowNs();
        SetFirstIfEmpty(first_request_sent_at_ns_, now_ns);
        last_request_sent_at_ns_.store(now_ns, std::memory_order_relaxed);
    }

    void UpdateResponseSent()
    {
        responses_sent_.fetch_add(1, std::memory_order_relaxed);
    }

    void UpdateRequestReceived()
    {
        requests_received_.fetch_add(1, std::memory_order_relaxed);

        const int64_t now_ns = NowNs();
        SetFirstIfEmpty(first_request_received_at_ns_, now_ns);
        last_request_received_at_ns_.store(now_ns, std::memory_order_relaxed);
    }

public:
    uint32_t PacketsSent() const
    {
        return packets_sent_.load(std::memory_order_relaxed);
    }

    uint32_t PacketsReceived() const
    {
        return packets_received_.load(std::memory_order_relaxed);
    }

    uint64_t BytesSent() const
    {
        return bytes_sent_.load(std::memory_order_relaxed);
    }

    uint64_t BytesReceived() const
    {
        return bytes_received_.load(std::memory_order_relaxed);
    }

    void UpdatePacketSent(int n)
    {
        if (n <= 0)
            return;

        packets_sent_.fetch_add(1, std::memory_order_relaxed);
        bytes_sent_.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
        last_packet_sent_at_ns_.store(NowNs(), std::memory_order_relaxed);
    }

    void UpdatePacketReceived(int n)
    {
        if (n <= 0)
            return;

        packets_received_.fetch_add(1, std::memory_order_relaxed);
        bytes_received_.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
        last_packet_received_at_ns_.store(NowNs(), std::memory_order_relaxed);
    }

    void UpdateRoundTripTime(std::chrono::nanoseconds rtt)
    {
        const int64_t rtt_ns = rtt.count();

        current_round_trip_time_ns_.store(rtt_ns, std::memory_order_relaxed);
        total_round_trip_time_ns_.fetch_add(rtt_ns, std::memory_order_relaxed);
        responses_received_.fetch_add(1, std::memory_order_relaxed);

        const int64_t now_ns = NowNs();
        SetFirstIfEmpty(first_response_received_at_ns_, now_ns);
        last_response_received_at_ns_.store(now_ns, std::memory_order_relaxed);
    }

    double CurrentRoundTripTime() const
    {
        return static_cast<double>(
                   current_round_trip_time_ns_.load(std::memory_order_relaxed))
               / 1000000000.0;
    }

    double TotalRoundTripTime() const
    {
        return static_cast<double>(
                   total_round_trip_time_ns_.load(std::memory_order_relaxed))
               / 1000000000.0;
    }

public:
    std::optional<TimePoint> LastPacketSentAt() const
    {
        return NsToTimePoint(last_packet_sent_at_ns_.load(std::memory_order_relaxed));
    }

    std::optional<TimePoint> LastPacketReceivedAt() const
    {
        return NsToTimePoint(last_packet_received_at_ns_.load(std::memory_order_relaxed));
    }

    std::optional<TimePoint> FirstRequestSentAt() const
    {
        return NsToTimePoint(first_request_sent_at_ns_.load(std::memory_order_relaxed));
    }

    std::optional<TimePoint> LastRequestSentAt() const
    {
        return NsToTimePoint(last_request_sent_at_ns_.load(std::memory_order_relaxed));
    }

    std::optional<TimePoint> FirstResponseReceivedAt() const
    {
        return NsToTimePoint(first_response_received_at_ns_.load(std::memory_order_relaxed));
    }

    std::optional<TimePoint> LastResponseReceivedAt() const
    {
        return NsToTimePoint(last_response_received_at_ns_.load(std::memory_order_relaxed));
    }

    std::optional<TimePoint> FirstRequestReceivedAt() const
    {
        return NsToTimePoint(first_request_received_at_ns_.load(std::memory_order_relaxed));
    }

    std::optional<TimePoint> LastRequestReceivedAt() const
    {
        return NsToTimePoint(last_request_received_at_ns_.load(std::memory_order_relaxed));
    }

private:
    std::atomic<uint64_t> id_{0};

    IceCandidate local_;
    IceCandidate remote_;

    bool ice_role_controlling_{false};

    std::atomic<uint16_t> binding_request_count_{0};
    std::atomic<IceCandidatePairState> state_{IceCandidatePairState::Waiting};
    std::atomic<bool> nominated_{false};
    std::atomic<bool> nominate_on_binding_success_{false};

    std::atomic<int64_t> current_round_trip_time_ns_{0};
    std::atomic<int64_t> total_round_trip_time_ns_{0};

    std::atomic<uint32_t> packets_sent_{0};
    std::atomic<uint32_t> packets_received_{0};
    std::atomic<uint64_t> bytes_sent_{0};
    std::atomic<uint64_t> bytes_received_{0};

    std::atomic<int64_t> last_packet_sent_at_ns_{0};
    std::atomic<int64_t> last_packet_received_at_ns_{0};

    std::atomic<uint64_t> requests_received_{0};
    std::atomic<uint64_t> requests_sent_{0};
    std::atomic<uint64_t> responses_received_{0};
    std::atomic<uint64_t> responses_sent_{0};

    std::atomic<int64_t> first_request_sent_at_ns_{0};
    std::atomic<int64_t> last_request_sent_at_ns_{0};
    std::atomic<int64_t> first_response_received_at_ns_{0};
    std::atomic<int64_t> last_response_received_at_ns_{0};
    std::atomic<int64_t> first_request_received_at_ns_{0};
    std::atomic<int64_t> last_request_received_at_ns_{0};

private:
    static bool CandidateEqual(const IceCandidate& a, const IceCandidate& b)
    {
        return a.Foundation() == b.Foundation() &&
               a.Component() == b.Component() &&
               a.Transport() == b.Transport() &&
               a.Type() == b.Type() &&
               a.Priority() == b.Priority() &&
               a.Address() == b.Address() &&
               a.BaseAddress() == b.BaseAddress() &&
               a.RelatedAddress() == b.RelatedAddress();
    }

    static int64_t NowNs()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   Clock::now().time_since_epoch())
            .count();
    }

    static std::optional<TimePoint> NsToTimePoint(int64_t ns)
    {
        if (ns == 0)
            return std::nullopt;
        return TimePoint(std::chrono::nanoseconds(ns));
    }

    static void SetFirstIfEmpty(std::atomic<int64_t>& field, int64_t value)
    {
        int64_t expected = 0;
        field.compare_exchange_strong(expected, value, std::memory_order_relaxed);
    }
};


} // namespace ice



#endif /* _ICE_H_ */