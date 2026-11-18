#pragma once

#include "TransportDiagnostics.h"
#include "../transport/PacedSendQueue.h"
#include "LogRecord.h"
#include <functional>
#include <tuple>

namespace media
{
bool QosTraceEnabled() noexcept;
void WriteQosEvent(const char* event, const packetia::logging::LogFields& fields) noexcept;

// Owned by the downstream media worker. No packet payloads or peer credentials.
class QosTrace
{
public:
    using Emit = std::function<void(const char*, const packetia::logging::LogFields&)>;
    explicit QosTrace(Emit emit = WriteQosEvent) : emit_(std::move(emit)) {}
    bool SampleDue(uint64_t now_ms) const;
    void Observe(uint64_t transport_id, const TransportDiagnostics& snapshot,
                 const transport::PacedSendQueue::Stats* queue = nullptr);

private:
    using ControlState = std::tuple<uint64_t, bool, bool, std::optional<BandwidthUsage>, std::string>;
    Emit emit_;
    std::optional<TransportDiagnostics> previous_;
    std::optional<ControlState> control_state_;
    std::optional<std::pair<uint64_t, int>> probe_result_;
};
}
