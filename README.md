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


# Version 0.3.3 - 2026-01-13
## feature: implement RTSP TCP interleaved demux and RTP worker dispatch

- Add RTSP interleaved ($) frame parsing in RtspConnection
- Bind interleaved channel to RtpTrack during SETUP (RTP/RTCP)
- Deliver interleaved RTP/RTCP payloads to media WorkerPool
- Use track-based key for per-track serial processing
- Integrate PacketPool for RTP payload buffering

tips:I'm tired today, going to rest and then continue studying BBR and Gerrit
This establishes the complete TCP → channel → track → worker RTP pipeline.


# Version 0.3.4 - 2026-01-14
## fix: optimize RTP track map locking and reduce log noise

- Replace std::mutex with std::shared_mutex for RtpJobHandler track map
  to improve concurrency under high RTP load (read-heavy scenario)
- Use shared_lock for track lookup and unique_lock for track insert/erase
- Downgrade excessive ERROR logs to WARN for normal track teardown and race cases
- Improve log context to include key and payload length for easier troubleshooting

tips: I’m just figuring out how to complete my own RTP pipeline, mainly by following ZLMediaKit’s design.
This change avoids unnecessary lock contention in RTP worker threads
and prevents log flooding during track lifecycle transitions.

# Version 0.3.5 - 2026-01- 15
## feat(rtsp/rtp): introduce RTP wire header parsing and packet reorder pipeline

- Add RtpWireHeader to correctly parse RTP wire-format headers (RFC3550)
- Separate RTP wire header from logical RtpHeader to avoid ABI/layout bugs
- Refactor RtpVideoTracker::inputRtp to parse PT/SSRC/SEQ/Timestamp from wire header
- Integrate EnhancedPacketSortor to handle RTP reordering and wrap-around
- Prepare hooks for PT/SSRC locking and NTP-based timestamp conversion
- Lay groundwork for TCP-interleaved and UDP unified RTP input pipeline

tips: I'm just learn how to work jitter buffer

This change fixes incorrect RTP header parsing, prevents random seq/pt/ssrc
misinterpretation, and enables stable real-time packet reordering.

# Version 0.3.6 - 2026-01- 16
## refactor(rtsp): rework RTP over TCP interleaved dispatch and job handling

- Introduce channel-to-track binding for RTP/RTCP interleaved streams
- Pass weak_ptr<RtpTrack> through WorkJob instead of relying on handler-side maps
- Clarify ownership and lifetime between interleaved parser and worker threads
- Prepare worker path for safe RTP packet processing

Note:
Further investigation is required for packet lifetime and use-after-free issues.

# Version 0.3.7 - 2026 - 01 - 19
## fix: resolve RTP packet lifetime issues in worker pool

- Fix heap-use-after-free caused by incorrect ownership of RTP packet memory
- Ensure Packet lifetime is managed consistently across worker threads
- Clarify WorkJob payload semantics (Packet* vs raw buffer)
- Avoid double-free by unifying packet release responsibility
- Improve robustness of RtpJobHandler::handle under concurrent execution

# Version 0.3.8 - 2026-01-20
## rtp: wire RTP parsing into worker → track pipeline
- Add RTP raw packet handling in RtpJobHandler
- Parse and validate RTP headers before track processing
- Construct RtpPacket from raw bytes and hand off to RtpTrack
- Prepare track-level pipeline for ordered RTP processing
- Lay groundwork for jitter buffer and depacketizer integration


# Version 0.3.9 - 2026-01-21
## refactor: add RTCP packet type enums and feedback definitions
- Define RTCP packet type enums (SR/RR/SDES/BYE/APP/RTPFB/PSFB/XR)
- Add SDES item type definitions per RFC3550
- Add RTPFB/PSFB feedback type enums (NACK/PLI/FIR/REMB, etc.)
- Use strongly-typed enum class with protocol-aligned values
- Prepare groundwork for RTCP parsing and statistics handling


# Version 0.4.0 - 2026-01-23
## refactor: rework RTP job payload model and fix abort caused by invalid length'
- Replace void* payload with std::variant
- Use shared_ptr<Packet> for safe cross-thread ownership
- Remove unsafe static_cast paths
- Fix len=0 propagation to inputRtp()

# Version 0.4.1 - 2026-01-27
## feat(rtp): implement RTP parsing, jitter buffering and H264 depacketization

- Improve RTP header parsing and validation
- Integrate jitter buffer for packet reordering
- Implement ordered RTP callback
- Support H264 Single / STAP-A / FU-A depacketization
- Output Annex-B formatted frames

# DEBUG - 2026-01-28
## debug: verify RTP pipeline end-to-end and locate PacketPool exhaustion path

- Verified full RTP flow from worker dispatch to RtpVideoTracker::onRtpSorted
- Confirmed callback and sorting stages are correctly triggered
- Identified potential lifetime and caching issues causing PacketPool exhaustion

# Docs - 2026-01-31
## docs: add RTCP XR (RFC 3611) protocol and SDP signaling notes
- Document RTCP XR common header (PT=207) and packet structure
- Describe the XR report block model and usage scenarios
- Clarify sequence-number–based reporting for Duplicate RLE and Packet Receipt Times blocks
- Explain RTT measurement using Receiver Reference Time (BT=4) and DLRR (BT=5)
- Add SDP a=rtcp-xr signaling rules, distinguishing unilateral and collaborative parameters
- Clarify Offer/Answer behavior and bandwidth considerations for XR usage

This change only updates protocol documentation and design notes, without affecting
existing RTP/RTCP data path logic.

# ROOM:20260204
## feat: introduce ClientSession for per-client send handling

- Isolate per-client RTP send queue and state into ClientSession
- Prepare MediaSession for SFU-style multi-subscriber forwarding

# RoomVersion:0.0.1: 2026-02-05
## feat: add basic SFU room

- Introduce Room module to manage participants and provide basic conference routing.
- Add participant join/leave management and broadcast forwarding logic.
- Each incoming RTP packet is forwarded to all other participants (N-1 fanout).
- Prepare foundation for future subscribe-based routing / simulcast / SVC.

# 2026-02-06
## feat(rtsp/rtp): add RTP packet sorting logs and improve debug tracing
- Add detailed debug logs for RTP sorting pipeline (EnhancedPacketSortor)
- Print seq/next_seq/buffer state to verify jitter-buffer reorder behavior
- Add gdb breakpoint tracing points for emit() / inputRtp() call path
- Improve packet dump helper to validate RTP header correctness
- Facilitate troubleshooting for RTP packet payload/ts/seq parsing issues

# Version 0.4.2 - 2026-02-08
## feat(core): add signal-based subscription framework for stream dispatch
- Add ISubscription / ISignal interfaces for callback subscription model
- Implement SignalCOW with copy-on-write snapshot for lock-free emit path
- Provide subscribe/cancel mechanism to manage listener lifecycle
- Introduce SourceBase<T> abstraction to expose publish/subscribe pattern for stream modules
- Prepare foundation for RTP/frame fan-out and modular pipeline extension

# Version 0.4.3 -  2026-02-09
## Add Depacketizer module and start H264 RTP frame reassembly

- Introduced a new Depacketizer module and defined a unified interface (input() / hasFrame() / popFrame()) for frame-level reconstruction based on sorted RTP packets.
- Added initial H264Depacketizer class skeleton, preparing the architecture for future codec extensions (H265/VP8, etc.).
- Completed RTP packet metadata filling after parsing, including ts/marker/version/padding/extension/cc/hdr_len/payload_off/payload_len, ensuring downstream depacketization can rely on correct header/payload boundaries.
- Improved RTP header parsing to correctly handle CSRC and header extensions, with additional validation for padding scenarios.
- Added debug logs for RtpSorted output to verify sequence continuity and payload size variations, confirming correct behavior before implementing FU-A/STAP-A reassembly logic.