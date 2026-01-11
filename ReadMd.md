## Version 0.2.0 – 2025-12-19

### Highlights
This release refactors RTSP over TCP handling by separating RTP/RTCP interleaved
media packets from RTSP control message parsing, significantly improving
stability and correctness.

### Improvements
- Demultiplex RTP/RTCP interleaved packets before RTSP parsing
- Correctly handle TCP sticky packets and fragmented interleaved frames
- Avoid treating empty buffers and partial packets as connection errors

### Bug Fixes
- Fix RTSP parser being disrupted by interleaved RTP packets
- Fix incorrect connection termination on empty read buffer

### Design Changes
- Introduce a clear separation between media data plane and RTSP control plane
- RTP/RTCP packets are now consumed at the connection read level

# Version 0.3.0 – 2025-12-30
## Highlights

This release completes the RTSP-over-TCP interleaved media pipeline by
introducing explicit channel-to-track binding and a codec-aware RTP track
factory, enabling correct RTP/RTCP demultiplexing and per-track packet handling.

## Improvements

- Introduce RtpInterleaved channel map to dispatch interleaved RTP/RTCP packets
by negotiated TCP channel

- Bind RTP/RTCP interleaved channels to tracks during SETUP negotiation

- Add createTrack() factory to construct codec-specific RTP tracks

- Parse a=control:streamid=N safely via a dedicated ParseStreamId() helper

- Improve RTP debug visibility with optional per-packet header logging

- Generate compile_commands.json via CMake for accurate IDE tooling support

## Bug Fixes

- Fix incorrect use of track index as interleaved channel ID

- Fix RTP/RTCP packets being routed without validated SETUP binding

- Fix potential session/track mismatch during multi-track SETUP sequences

- Fix undefined reference caused by missing function definitions at link time

## Design Changes

- Clearly separate RTSP control plane (request/response parsing)
from media data plane (RTP/RTCP packet dispatch)

- RtpTrack lifetime is now managed via shared_ptr, while interleaved dispatch
uses weak_ptr to avoid ownership cycles

- TCP interleaved channels are treated as connection-scoped identifiers,
independent of media track indices


# Version 0.2.1 – 2026-01-01
## Highlights

- This release fixes RTSP RECORD failures caused by missing track registration during SETUP, and introduces an initial - - RTP-over-TCP interleaved processing pipeline that decouples network I/O from media processing.

## Improvements

- Register SDP tracks into MediaSession::tracks_ during SETUP to ensure RECORD operates on a fully initialized session

- Add MediaSession::BindRtpTrack(trackIdx, track_ptr) to associate trackIdx -> RtpTrack for deterministic lookup during - - RECORD and media processing

- Introduce an initial worker-based RTP input pipeline (PacketPool + WorkerPool) to offload heavy RTP processing from the connection read thread

- Add structured logging for SETUP track matching (control/codec/pt/clock/trackIdx) and for RECORD session/track validation

## Bug Fixes

- Fix RTSP RECORD returning no response when MediaSession::tracks_ is empty (root cause: SETUP did not register tracks)

- Fix incorrect/misleading logs in RECORD handler (e.g., printing “SETUP request” in RECORD path)

# Version 0.3.0 – 2026-01-02
## Highlights

- This release introduces a reusable real-time packet delivery framework for RTP/RTCP processing. It adds a fixed-capacity PacketPool for deterministic memory management and a dedicated SPSC RtpRingBuffer (with RTCP priority) to decouple I/O from media processing, improving stability under load.

## Improvements

- Add PacketPool (object/memory pool) to provide fixed-capacity packet buffers and avoid frequent heap allocations in the RTP data path

- Add RtpRingBuffer built on SPSC rings, supporting separate RTP/RTCP queues with RTCP-first dequeue policy

- Refactor interleaved RTP ingestion to a “copy + enqueue” model, simplifying the read thread responsibilities and reducing lifetime hazards

- Introduce queue/pool observability hooks (queue depth, dropped/exhausted counters) to facilitate load testing and tuning

## Bug Fixes

- Fix potential lifetime issues when dispatching interleaved RTP/RTCP across threads by avoiding raw pointer ownership leaks and centralizing buffer release

- Resolve const-correct locking issue in PacketPool by making internal mutex mutable (enables thread-safe const observers such as size()/stats())


# Version 0.3.1 – 2026-01-06
## refactor(network): fix connection lifetime issues and introduce factory-based initialization

- Introduce two-phase initialization for TcpConnection (construct + Start)
- Move channel registration and event enabling out of constructors
- Add factory method RtspConnection::Create to enforce correct init order
- Replace raw `this` captures in callbacks with weak_ptr to prevent UAF
- Eliminate bad_weak_ptr caused by shared_from_this in constructors
- Unify RTSP connection creation through server OnConnect factory path
- Improve disconnect handling to avoid delayed callback accessing destroyed server


# Version 0.3.2 – 2026-01-07


## Architecture Changes

- Introduce a generic ShardedWorkerPool with key-based sharding to guarantee
per-key ordering while allowing parallel execution across workers.

- Add a pluggable IJobHandler interface, enabling different subsystems
(RTP, RTCP, media processing, future non-RTSP tasks) to reuse the same worker pool.

- Refactor RTP processing into a clear producer → dispatcher → consumer model:

- IO thread: framing and minimal copy only

- Worker threads: RTP/RTCP consumption and track-level processing

- Decouple RTP consumption logic from RTSP, making the worker service reusable
by other modules.

## RTP / RTSP Improvements

- Implement a clean RTSP over TCP interleaved handling model:

- Strict $ <channel> <length> <payload> framing

- Robust handling of sticky packets and fragmented frames

- Add explicit channel → track binding established during RTSP SETUP.

- Ensure RTP/RTCP packets are never parsed by the RTSP control parser.

- Move all heavy RTP processing out of IO threads to worker threads.

## Worker & Scheduling

- Add queue depth limits and drop policies (DropHead / DropTail) to protect
the system under load.

- Guarantee in-order processing for the same track_id via consistent sharding.

- Provide safe shutdown semantics with optional queue draining.

- Improve statistics collection for enqueue, dequeue, drops, and max queue depth.

## Memory & Stability

- Centralize packet lifetime management via PacketPool.

- Ensure all dropped or unbound jobs properly release packet memory.

- Eliminate potential memory leaks when queues overflow or tracks are unbound.

- Improve defensive checks for invalid channels and oversized RTP packets.

# Version 0.3.3 – 2026-01-11
## fix(workerpool): fix PacketPool leak by restoring job-based release chain

Fix a critical memory leak where PacketPool objects were never released
after being consumed by ShardedWorkerPool workers.

Root cause:
- Packet ownership was lost during job dispatch.
- job.deleter() was invoked, but Packet::owner was null, so PacketPool::release()
  was never called.
- Additionally, job drop paths (queue full, post failure, DropHead) were
  not releasing payloads, causing pool exhaustion.

This patch:
- Restores a strict ownership chain: Packet -> WorkJob -> deleter -> PacketPool
- Ensures all failure / drop paths in ShardedWorkerPool::post() call job.deleter()
- Guarantees that every Packet acquired from PacketPool is eventually released
  exactly once.

After this change:
- PacketPool stats (acquired / released) remain balanced under load
- PacketPool no longer exhausts under sustained RTP traffic
- WorkerPool becomes memory-safe under backpressure and drops

This fixes frequent RTSP/RTP failures (-12) caused by pool exhaustion.

