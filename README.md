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

# Version 0.4.4 - 2026-02-14
## Introduce RTCP module architecture and prepare RTP/RTCP interleaved processing

- Added initial RTCP module framework following a WebRTC-style interface design, including IRtcpReceiver, IRtcpSender, and IRtcpObserver for clean protocol/business separation.
- Implemented RtcpReceiverImpl skeleton with core entry points (OnRtcpPacket, SetObserver, SetLocalSsrc, SetRemoteSsrc) to prepare for compound RTCP parsing.
- Confirmed RTSP interleaved transport behavior where RTP and RTCP share the same TCP socket, and identified channel-based demux logic (interleaved=RTP-RTCP mapping).
- Prepared media pipeline integration points for routing interleaved RTCP payloads into the RTCP receiver, enabling future support for RR/NACK/PLI feedback handling.
- Reviewed RtspConnection initialization flow and verified worker pool (media) and packet pool integration to support upcoming RTCP parsing and retransmission work.


# Version 0.4.5- 2026-02-15
## Title: Add RTCP receiver integration and fix build/link issues in RTP track
- Introduced RTCP handling interface (inputRtcp) in RtpTrack and implemented RTCP callback mechanism via IRtcpObserver.
- Integrated RtcpReceiverImpl into RtpVideoTracker to support parsing RTCP packets (RR/NACK/PLI events reserved).
- Fixed namespace and constructor signature mismatch for RtcpReceiverImpl (rtcpx::IRtcpObserver*) to resolve undefined reference issues.
- Updated CMake build linkage to ensure RTCP implementation is correctly compiled and linked for unit tests.
Improved RTSP/RTP module structure preparing for future RTCP feedback processing and keyframe request support.

# Version 0.4.6 - 2026-02-16
## Refactor TCP stack by introducing a Session/Observer based architecture.
- Added generic ICodec<Msg> interface to support protocol-level decoding/encoding (SIP/RTMP/RTSP, etc.).
- Implemented ObserverList to broadcast decoded messages to multiple business modules via ISessionObserver.
- Introduced IConnectionObserver and improved connection-to-session callback flow for byte-level events.
- Updated TcpConnection to expose bytes callbacks so TcpSession can take ownership of protocol parsing logic.
- Prepared the framework for multi-protocol session management with clean transport/protocol separation.

tips: Today is Chinese New Year. I stayed in my rented apartment and spent the day coding.


## Commit Message (2026-02-17)
Refactored multi-protocol TCP session architecture by introducing ProtocolDetector and ProtocolDetectorSession, enabling dynamic protocol detection and seamless promotion to protocol-specific sessions (SIP/RTSP), while fixing include dependency and compilation issues in SIP parser integration.

# Version 0.4.7 - 2026-02-18
## Refactored protocol detection and RTSP parser integration by introducing RtspProtocolParser based on ProtocolParser
- Added RtspProtocolParser implementation based on ProtocolParser to support RTSP protocol detection (including $ interleaved framing).
- Refactored parser declarations/definitions and fixed ParseResult scope + missing return issues to resolve compilation errors.
- Updated build integration and linkage to eliminate duplicate Parse declarations and vtable undefined reference errors.

# Version 0.4.8 - 2026-2-20
## Refactor: Introduce protocol factory and session promotion mechanism
- Added ISessionFactory abstraction and integrated factory injection into TcpServer, enabling protocol-level session creation without coupling transport layer to specific protocol implementations.
- Implemented dynamic session promotion in ProtocolDetectorSession: upon successful protocol detection, create concrete session (e.g., RTSP) via factory and replace current session mapping.
- Optimized detection flow to ensure newly promoted session immediately processes existing buffer data, preventing first-packet loss during protocol switch.
- Decoupled RtspSession from RtspServer to reduce strong dependencies and improve modularity of protocol layer.

# Version 0.4.9 - 2026-2-22
## feat(network): introduce UDP server abstraction integrated with EventLoop
- Added UdpSocket encapsulation for non-blocking UDP operations (create/bind/recvfrom/sendto).
- Implemented UdpServer with Channel-based integration into existing Reactor (EventLoop + EpollTaskScheduler).
- Introduced IUdpHandler interface for decoupled datagram processing.
- Enabled TCP and UDP servers to coexist under the same EventLoop.
- Prepared foundation for future RTP/RTCP/ICE integration.

# Version 0.5.0 - 2026-2-23
## feat(media): introduce UDP transport skeleton and session demux layer
- Add MediaEngine as transport entry container to manage UdpServer lifecycle
- Implement UdpMuxHandler to demultiplex STUN / RTP / RTCP / DTLS packets
- Introduce UdpSession abstraction to encapsulate per-peer state
- Define initial session lookup/create mechanism (src-based mapping, preparatory for ICE-lite integration)
- Refactor ownership model: UdpServer uses unique/shared ownership at engine level; sessions hold non-owning reference
- Prepare groundwork for future ICE-lite + RTP integration

# Version 0.5.1 - 2026-2-24
## udp: add peer-based session routing and protocol demux
- Introduce peer -> UdpSession routing in UdpMuxHandler
- Enhance SocketAddr with operator== and custom hash
- Integrate selected peer binding mechanism

wip: unify C++17 standard and fix clangd remote configuration

- Ensure all targets compile with -std=gnu++17
- Fix nested namespace C++17 warning in STUN module
- Install and configure clangd-17 in remote SSH environment
- Explicitly set clangd.path and compile-commands-dir
- Regenerate and link compile_commands.json

Status: build ok, clangd indexing restored

WIP: scaffold ICE/STUN message layer and refactor RTSP parsing

- add initial StunMessage class skeleton (RFC8489 based)
- define STUN header layout (magic cookie, transaction id, attr parsing draft)
- introduce basic AttrType / MsgType enums
- start designing ICE candidate abstraction (placeholder only)

- refactor RTSP request parsing state machine
  * split request line / header / body stages
  * improve CRLF detection logic
  * prepare ANNOUNCE handling entry

- adjust logging granularity for protocol layer
- minor CMake cleanup

NOTE:
ICE connectivity check not implemented yet.
Attribute parsing currently incomplete (no integrity/fingerprint validation).

## 2026-07-26 — RTP 分轨绑定与 Tracker 内存管理

### 本次完成

- 修正 SDP 音视频 Track 与 Payload Type 的绑定逻辑。
- 按具体 media track 查询 PT，避免多路流及音视频重复 PT 相互覆盖。
- 支持绝对、相对 RTSP `a=control` 地址匹配。
- 支持静态音频 PT 0/8（PCMU/PCMA）。
- 检测重复 control，避免后注册 Track 静默覆盖已有 Track。
- 每个 RTP Tracker 使用独立、有界的 Packet Pool。
- 每个 Tracker 默认预分配 64 个 RTP Packet，复用 payload 内存。
- 音频、视频及不同 SSRC 的 Packet Pool 和排序 Buffer 相互隔离。
- 增加内存池耗尽、超大 RTP 包统计。
- 严格限制 RTP 乱序缓存大小，并增加超时清理。
- 补充 SDP Track 绑定、Packet Pool 和排序缓存回归测试。

### 当前 RTP 上行链路

```text
RTSP interleaved 接收
  → channel / Endpoint 分轨
  → SDP Track 与 PT 校验
  → 按 SSRC 创建 Audio/Video Tracker
  → Tracker 独立 Packet Pool
  → RTP 排序 Buffer
  → 音视频 Depacketizer
  → EncodedFrame
```

### 当前弱网与 RTCP 状态

- RTP 有界排序、基本丢包发现和发送端 RTP 重传缓存已经具备。
- RTCP SR/RR、NACK、PLI/FIR、Transport-CC 和 BWE 解析或算法组件已经存在。
- 接收端 NACK 生成、`RtcpDispatcher` 注册、音频 RTCP、PLI/FIR 转发及 BWE 控制动作尚未接入真实媒体链路。
- 下一阶段将优先完成：
  1. RTP 缺包检测 → RTCP NACK → RTP Cache 重传闭环。
  2. SR/RR、RTT、Jitter 与丢包率统计。
  3. PLI/FIR 关键帧恢复。
  4. Transport-CC、BWE 与弱网码率控制。

### 后续待完善

- 修复 RTP 网络入口超过 1500 字节时可能发生的静默截断。
- 减少 `WorkJob → Packet → Tracker Pool` 的中间内存复制。
- 将 `EncodedFrame` 接入录制、播放、转码或其他业务消费 Buffer。
- 接通原始 RTP SFU 订阅转发链路。

## 2026-07-26 — RTP Track Binding and Tracker Memory Management

### Completed

- Fixed SDP audio/video track and Payload Type binding.
- Scoped PT lookup to the corresponding media track, preventing collisions
  between multiple streams or audio/video tracks that reuse the same PT.
- Added support for matching both absolute and relative RTSP `a=control` URLs.
- Added support for static audio PT 0/8 (PCMU/PCMA).
- Added duplicate control detection to prevent a later track from silently
  replacing an existing track.
- Added an independent, bounded Packet Pool to every RTP Tracker.
- Each Tracker preallocates 64 RTP packets by default and reuses payload memory.
- Audio, video, and different SSRCs now have isolated Packet Pools and reorder
  buffers.
- Added counters for Packet Pool exhaustion and oversized RTP packets.
- Enforced the RTP reorder-buffer limit and added timeout-based cleanup.
- Added regression tests for SDP track binding, Packet Pool reuse, and reorder
  buffer limits.

### Current RTP Ingress Pipeline

```text
RTSP interleaved input
  → channel / Endpoint track routing
  → SDP Track and PT validation
  → Audio/Video Tracker creation per SSRC
  → per-Tracker Packet Pool
  → RTP reorder buffer
  → audio/video depacketizer
  → EncodedFrame
```

### Current Weak-Network and RTCP Status

- Bounded RTP reordering, basic packet-loss detection, and the sender-side RTP
  retransmission cache are available.
- Parsing or algorithm components exist for RTCP SR/RR, NACK, PLI/FIR,
  Transport-CC, and BWE.
- Receiver-side NACK generation, `RtcpDispatcher` registration, audio RTCP,
  PLI/FIR forwarding, and BWE control actions are not yet connected to the
  production media pipeline.
- The next phase will prioritize:
  1. Closing the RTP loss detection → RTCP NACK → RTP cache retransmission loop.
  2. Integrating SR/RR, RTT, jitter, and packet-loss statistics.
  3. Adding PLI/FIR-based key-frame recovery.
  4. Connecting Transport-CC, BWE, and weak-network bitrate control.

### Remaining Work

- Fix possible silent truncation of RTP packets larger than 1500 bytes at the
  network ingress boundary.
- Reduce intermediate memory copies across
  `WorkJob → Packet → Tracker Pool`.
- Connect `EncodedFrame` output to recording, playback, transcoding, or another
  application-level consumer buffer.
- Connect the raw RTP SFU subscription and forwarding pipeline.

## 2026-08-08 — 收紧媒体传输接口并统一 RTSP RTP 接收链路

### 本次完成

- 统一 `IMediaTransport` 的发送、接收、连接状态和生命周期接口。
- 新增 `ReceivedMediaPacket`，使用拥有型负载保证接收包能够安全跨线程传递。
- 新增 `IMediaPacketSource` 和 `IMediaPacketSink`，分离网络接收与媒体消费。
- 新增 `MediaTransportBase`，统一管理 Transport ID、原子状态和线程安全的 Sink。
- 实现 `RtspInterleavedTransport`，负责 RTP/RTCP channel 映射及 `$` 帧封装。
- 实现 `UdpMediaTransport`，负责 selected peer 校验和 UDP 数据发送。
- 新增 `MediaEndpointIngress`，统一 endpoint、SSRC、worker affinity 和媒体任务投递。
- 将 RTSP interleaved RTP/RTCP 接收接入 Transport/Ingress 链路。
- 使用 `WorkJob::owner` 管理异步任务引用的接收包内存，移除手工 `new[]/delete[]`。
- 将 RTCP NACK/PLI 发送改为通过 Transport 投递回 TCP IO 线程。
- 在 RTSP 断连和 `TEARDOWN` 时关闭并清理对应 Transport。
- 修复相关 CMake include 依赖和 TCP 接收日志变量错误。

### 当前接收链路

```text
TcpConnection
  → RtspSession（RTSP / '$' 分帧）
  → RtspInterleavedTransport
  → ReceivedMediaPacket
  → MediaEndpointIngress
  → media worker
  → SfuEndpoint / RTP Track
```

### 后续工作

- 由信令层建立 `UdpSession → SfuEndpoint` 的明确绑定后，将 UDP RTP/RTCP 接入同一个 `MediaEndpointIngress`。
- 减少 `MediaEndpoint::OnRtp()` 到 Tracker Packet Pool 之间的剩余内存复制。
- 接通 Room/SFU 的订阅发送链路，使 `RtpSenderTrack` 通过具体 Transport 完成下行转发。

## 2026-08-15 — 接入 RTCP 接收统计与弱网质量评估链路

### 本次完成

- 扩展 `RtpRecvStatsBase`，统一维护每个 SSRC 的 RTP/RTCP 接收统计。
- RTP 到达时实时统计包数、负载字节数、序列号回绕、重复包、乱序包和 RFC 3550 jitter。
- 根据接收时间和累计负载字节数计算平均接收码率。
- SR 到达时保存 NTP/RTP 时间映射、发送包数和发送字节数。
- 根据连续 SR 计算发送端负载码率、发送包速率、SR 间隔、实测 RTP 时钟频率和时钟漂移。
- 实现 `BuildReceiverReport()`，计算 RR report block 所需的 `fraction lost`、`cumulative lost`、`extended highest sequence`、`jitter`、`LSR` 和 `DLSR`。
- 将 `RtpRecvStatsBase` 下沉到 `RtpReceiverTrack`，使音频和视频轨使用同一套统计逻辑。
- 增加 `RtcpDispatcher` 的 SR 完成回调，在 Track 更新完成后通知对应 `SfuEndpoint`。
- Endpoint 根据媒体 SSRC 找到接收轨，将统计结果转换为 `WeakNetFeedback` 并提交给 `WeakNetController`。
- 按 SSRC 独立保存弱网控制器、最近一次 RR 统计结果和网络质量等级，避免音视频状态互相覆盖。
- 视频网络质量进入 `Bad` 状态时，通过现有 RTCP Transport 链路发送 PLI 请求关键帧。
- 网络质量等级发生变化时输出 `[WEAK_NET] quality changed` 日志。
- 补充 SR/RR 接收日志，输出 NTP、RTP timestamp、包数、字节数及 report block 数量。
- 移除 AI Frame 和 Frame Router 的高频逐帧信息日志，保留队列溢出错误日志。
- 补充 RTCP 指标计算和 Dispatcher SR 回调单元测试。
- 补充 `RtpRecvStatsBase` 接口、参数单位、调用时机及状态副作用说明。
- 修复 `OnSenderReport` 调用名称不一致导致的编译错误。

### 当前计算链路

```text
RTP packet
  → RtpReceiverTrack::inputPacket()
  → RtpRecvStatsBase::OnRtpPacket()
  → sequence / loss / jitter / receive bitrate

RTCP SR
  → RtcpReceiverImpl
  → RtcpDispatcher::OnSenderReport()
  → RtpReceiverTrack::OnRtcpSenderReport()
  → TrackClock + RtpRecvStatsBase::OnSenderReport()
  → SfuEndpoint::EvaluateReceiveQuality()
  → BuildReceiverReport()
  → WeakNetFeedback
  → WeakNetController::OnFeedback()
  → NetworkControlUpdate
```

### 后续工作

- 增加独立的质量评估周期，避免完全依赖上游 SR 的发送周期。
- 周期构造并发送 RTCP RR，复用弱网评估已经生成的 report block，避免重复推进统计区间基线。
- 将目标码率、Pacer 和 FEC 建议接入实际媒体发送或编码控制接口。
- 完善下行 `RtpSenderTrack` 的 RR/TWCC、RTT 和带宽估计控制链路。
- 增加 Endpoint/Session 级音视频质量聚合策略。
