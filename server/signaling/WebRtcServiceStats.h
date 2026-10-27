#pragma once

#include <cstddef>
#include <cstdint>

namespace server
{
// Snapshot collected on the service executor; no internal synchronization.
// Counters and timings persist across Stop/Start on the same service instance.
// Durations include failed requests but exclude queueing and network I/O.
struct WebRtcServiceStats
{
    std::size_t active_signaling = 0;           // Registered sessions, including pending and closing sessions.
    std::size_t pending_admissions = 0;         // Awaiting join/resume initial response confirmation; excludes closing.
    std::size_t reconnecting_participants = 0;  // Disconnected participants retained for recovery.

    uint64_t signaling_opened = 0;             // Total sessions registered.
    uint64_t signaling_closed = 0;             // Total sessions removed, including shutdown cleanup.
    uint64_t admission_timeouts = 0;           // Initial response deadline exceeded, including idle admissions.

    uint64_t join_attempts = 0;                // Total join requests processed.
    uint64_t join_succeeded = 0;               // Participant bindings created; later rollback does not decrement.
    uint64_t join_failed = 0;                  // Join validation or binding failures.

    uint64_t resume_attempts = 0;              // Total resume requests processed.
    uint64_t resume_succeeded = 0;             // Signaling bindings restored; media recovery is not implied.
    uint64_t resume_failed = 0;                // Resume validation or binding failures.

    uint64_t admission_validation_failed = 0;  // Invalid parameters, identity, permissions, or admission policy.
    uint64_t admission_binding_failed = 0;     // Room binding failures after validation succeeds.

    uint64_t join_duration_us_total = 0;       // Total join validation and room processing time, in microseconds.
    uint64_t join_duration_us_max = 0;         // Maximum join processing time, in microseconds.
    uint64_t resume_duration_us_total = 0;     // Total resume validation and binding time, in microseconds.
    uint64_t resume_duration_us_max = 0;       // Maximum resume processing time, in microseconds.

    uint64_t recovery_expired = 0;             // Participants removed after the recovery window expires.
    uint64_t negotiation_timeouts = 0;         // Server-initiated negotiations timed out and rolled back.
    uint64_t media_closed = 0;                 // Failed/closed/timed-out media cleanup; excludes leave and shutdown.
};

} // namespace server
