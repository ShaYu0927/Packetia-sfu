# 开发记录

项目使用方式见[根 README](../README.md)，详细实现说明见[技术参考](reference.md)。
历史验证结果仅说明对应版本的验证范围。

## 2026-11-06 — 处理服务接口与构建模块拆分

- 为录制、混流和转码增加服务接口，集中定义处理组件集合与默认本地组装。
- `ServerApp` 支持注入组件工厂，检查组件类型、初始状态和输出路由，并保留原有调用方式。
- 录制、AI、混流、转码分别声明构建目标；接口与默认组装分开，FFmpeg 依赖按目标传递。
- 主程序和应用集成测试共用应用运行库，保留现有程序名与产物路径。
- 重写根 README，将详细技术说明和历史记录分别归档。
- 当前配置全量构建通过；应用生命周期、配置、处理服务、转码、WebRTC 与会议控制等 11 组相关 CTest 通过。
- 处理组件仍在现有单进程入口内运行，外部处理进程和远程任务协议尚未实现。


以下记录描述各次修改当时的状态；其中的待实现能力和限制可能已在后续变化，当前状态以根 README 为准。

## 2026-10-30：整理帧分发与接收质量的模块边界

- 编码帧分发迁入 `media/frame` 和 `packetia_media_core`，解除对 `SfuEndpointTypes.h` 的依赖；帧回调重入测试归入媒体核心测试。
- `media/quality/ReceiveQualityMonitor` 仅接收统计、保存各源控制器并返回控制建议。
- `media/endpoint/ReceiveFeedbackHandler` 衔接 RTP 接收器、RR 发送和质量评估，保持原有反馈与关键帧路径。
- 端点类型头删除质量状态和帧回调声明；质量模块新增多源隔离、移除与清空后的状态重置测试。
- `Packetia`、`PacketiaControl`、`PacketiaSfu` 构建通过；媒体核心、弱网、RTP/组件、RTSP UDP、流服务和 WebRTC 共 6 组 CTest 全部通过。

## 2026-10-30：拆分 SFU 端点职责

- 下行参数、订阅和发送器生命周期移入 `DownlinkTrackManager`，端点保留源/目标加锁与跨端点通知。
- 编码帧来源、回调和发布交给 `EncodedFrameDispatcher`；接收报告与质量评估交给 `ReceiveQualityMonitor`。
- WebRTC 显式传入帧来源，删除只为修改内部字段而存在的 `PublishedEndpoint` 派生类。
- 补充组件销毁后旧路由失效、RTCP 注册撤销、帧回调重入和实际音频收包后 RR 身份的回归测试。
- `Packetia`、`PacketiaControl`、`PacketiaSfu` 构建通过；媒体核心、RTP/组件、RTSP UDP、流服务和 WebRTC 共 5 组 CTest 全部通过。
- 保留现有客户端协议与执行器归属；发布协商和接收器管理仍由端点协调。

## 2026-10-30：编码帧转发准入、索引与关闭

- 编码帧路由增加可配置订阅上限、明确准入结果和失败回滚，保留旧订阅接口。
- 按发布实例与轨道建立独立索引；同一流 256 条轨道的测试中，单帧仅检查目标轨道及三条通用关系，共 4 个候选。
- 新增幂等关闭与累计投递统计；旧快照会检查关系是否失效，控制路径在锁外释放回调捕获，允许重入。
- 增加满额、过期回收、回调异常、关闭并发、失败清理及析构重入测试。
- `Packetia`、`PacketiaControl`、`PacketiaSfu` 构建通过；媒体核心、RTP、WebRTC、转码、混流和流服务 6 组 CTest，以及独立转码构建的 3 组 CTest 全部通过。
- 本次范围为编码帧路由，不改变直接 RTP 转发算法、消费者队列策略或客户端信令；未做容量与长稳压测。

## 2026-10-29：媒体层目录与轨道命名统一

- 区分源描述、发布信息对象和消费句柄，更新为 `MediaTrackDescription`、`TrackPublicationInfo`、`TrackPublication` 与 `MediaTrackSubscription`。
- 将帧类型和编码帧路由集中到 `media/frame`，将质量测试统一为 `media/quality/tests`，规范 BWE 文件名和相关头文件保护宏。
- 房间发布事件移至 `Room/TrackPublicationEvent.h` 与 `room` 命名空间；同步更新房间、信令、RTSP、录制、转码和混流引用。
- 更新主工程与独立构建的 CMake 源文件和头文件路径；媒体层规范集中记录在本 README。
- 目录命名检查覆盖 `media` 下 102 个 C++ 文件；三个主程序和 9 组相关 CTest 通过，独立转码 3 组、混流 1 组测试通过。
- 尚未验证商业容量、长稳、公网部署或新增协议互通；这些结果不作为生产就绪结论。

## 2026-10-29：全局轨道引用与消费控制

- 源流为 `MediaTrackDescription.ref` 赋予进程内全局引用，描述克隆到新发布时更换实例身份；消费者不重复注册源轨道。
- 注册表增加轨道索引、消费关系快照、源与单消费者暂停/恢复、解除消费、单轨道移除和可用性事件。
- 消费句柄使用原子闸门与弱引用登记，关闭先停止后续投递，再于管理器锁外通知清理；每条源轨道限制 4096 个存活句柄。
- 房间 RTP 订阅和受管理编码帧订阅接入控制；RTP 发送出口覆盖缓存重传，暂停保留发送器与消费关系。
- 全局移除通过脏标志回到 WebRTC owner，在原有房间链路中清理接收器、订阅与媒体会话并通知客户端，成员身份保留。
- 补充引用生成、未就绪拒绝、暂停隔离、关闭重入、容量限制、并发移除、真实 RTP/NACK 及 WebRTC owner 清理测试。
- `Packetia`、`PacketiaControl`、`PacketiaSfu` 构建通过；媒体核心、RTP、RTSP UDP、WebRTC、转码、混流及流服务 7 组 CTest 全部通过。
- RTSP 动态单轨道 owner 清理、旧全量帧订阅的任务联动、动态新增轨道、跨节点控制和容量压测仍未实现。

## 2026-10-29：编码帧按流与轨道路由

- 将编码帧路由归入 `packetia_media_core`，新增按发布实例、轨道或 Endpoint 等来源条件订阅，保留原有全量订阅。
- 使用不可变路由快照和来源索引；逐帧分发不再复制全部订阅列表，不持有路由写锁调用消费者。
- 新增接收、拒收、异常的投递结果，隔离消费者异常；退订使旧快照条目失效，已开始的回调仍由消费者入口和队列收尾。
- 转码任务接入来源过滤和内核发布实例 ID；真实 RTP 分片乱序组帧经过指定轨道路由，完成 12 帧输入到两档共 24 帧输出的验证。
- 清理已删除媒体测试的失效 CMake 引用，新增独立 `media_core` 测试；三个服务入口构建及 8 组相关 CTest 通过。
- 未新增全局轨道控制命令、源注销自动停止业务、跨协议输出或容量压测。

## 2026-10-29：统一发布流注册框架

- RTSP/WebRTC 发布统一进入 `MediaStreamRegistry`，按 scope/name 查找逻辑流，按发布实例 ID 和流内轨道 ID 解析轨道及 Endpoint。
- 补齐来源信息、全局值快照及注册/注销事件；房间业务轨道、编码帧关联同一内核发布实例，原有客户端信令字段保持不变。
- WebRTC 发布失败、取消发布、离会、恢复到期及停服注销流，resume/ICE restart 保留实例；订阅不重复注册发布流。
- 三个服务入口构建通过，9 组相关 CTest 通过，包含多 owner 注册并发、旧发布清理隔离、注册冲突回滚、RTSP UDP 收帧溯源和一控两 SFU 集成。
- 本次仅打通进程内注册框架；未实现 RTMP/SRT 接入、跨协议输出、全局轨道控制命令或跨节点流目录，未新增浏览器、公网和容量验证。

## 2026-10-29：会议控制面统一服务生命周期

- 新增 `MeetingModule : ServiceBase`，内部组合会议业务、准入、管理和共享节点路由；`ControlApp` 将会议和监控注册到 `ServiceManager`。
- 补齐 `ServiceBase` 生命周期实现和 `ServiceManager::MakeKey`，拆出独立的 `packetia_service_core`，新增 `ServiceType::Meeting`。
- 公共会议和内部节点 API 均经过模块准入检查；停止时拒绝新操作，等待已接收操作完成，HTTP worker 排空后再释放数据库。
- 数据库从构造阶段移至 `Init`，失败自动回滚；支持 `Stop → Start` 和 `Shutdown → Init → Start`，健康检查报告会议模块状态。
- 三个运行入口构建通过，11 个相关 CTest 入口通过，包含生命周期失败回滚、停止排空、旧路由保护及一控两 SFU 集成；未新增公网或容量验证。

## 2026-10-29：会议业务与节点调度拆分

- 将混合的 `ClusterController` 拆为公共 `RoomController`、内部 `NodeController` 和 SFU 的 `RoomControlController`。
- 抽出 `MeetingService`、`RoomAdmissionService`、`RoomAdminService`；`Coordinator` 只管理节点租约和房间绑定。
- 后台成员管理经过目标 SFU 的有界 owner 通道，复用 WebSocket 的资料校验、撤权、媒体回收与通知。
- 增加房间绑定代次、空房间回收与 `closing → ended` 流程；长期角色在下一次新入会生效，重连保留会内权限。
- 修复后端踢出最后一人时房间索引被删除导致的悬空引用；覆盖排队命令超时和迟到回收隔离。
- `Packetia`、`PacketiaControl`、`PacketiaSfu` 构建通过；9 个相关 CTest 入口全部通过，包含一控两 SFU 的真实进程测试。
- 本地启动脚本验证可生成 64 个节点的不冲突端口配置；这不代表已完成 64 节点运行或容量测试。

具体接口、线程归属、升级和失败语义见[会议控制与节点调度](reference.md#会议控制与节点调度)。

## 2026-10-23：服务端切网路径状态与媒体控制恢复

将切网路径状态接入核心 C++ 源码，使 ICE restart 或地址重绑定后，发送控制能切换到新路径的状态。

```text
WebRtcService → WebRtcSession → WebRtcMediaTransport → SendSideController
  restart 提交 → Checking → 新路径认证、提名及加密状态可用 → Ready
```

- 新增 `WebRtcNetworkPath`，描述 New、Checking、Ready、Failed、Closed，以及 ICE 代次、路径修订号和可用地址。
- `WebRtcSession` 在启动、restart 提交、路径可用、失败及关闭时通知上层；失败的协商准备保留原路径，
  同代次地址重绑定和同地址的新一轮 restart 均可更新路径，重复检查与重复提名不重复报告 Ready。
- `WebRtcService` 通过弱引用接通媒体适配器。等待或关闭期间禁用发送控制，Ready 时清除旧发送历史、
  反馈适配状态、TWCC 新鲜度和 GCC 估计；保留 TWCC 序号分配器、sender 登记及既有媒体会话身份。
- 异步发送携带路径 epoch，切网前已排队、尚未执行的任务检查代次后丢弃；移除 Send/收包时自行启用控制器的行为。
- 为可选 `log/Test` 和 `server/examples/WebRtcRoomDemo.cpp` 增加存在性检查，当前检出缺少这些文件时，
  核心程序与相关测试目标仍可生成。

验证：`Packetia` 及相关测试目标构建成功，六个 CTest 入口通过，覆盖会话、媒体适配、发送控制、业务服务、
真实 DTLS/SRTP 和 UDP mux。新增迁移用例检查旧凭据拒绝、重复提名、地址重绑定、同地址新代次恢复、
旧地址过滤、失败关闭、旧排队发送作废，以及旧反馈清理后 RR/新 TWCC 恢复。

```sh
cmake --build build/webrtc-integration \
  --target Packetia test_webrtc_session test_rtp_input test_weak_network_manual \
  test_webrtc_service test_webrtc_crypto test_datagram_transport -j4
ctest --test-dir build/webrtc-integration --output-on-failure \
  -R '^(test_webrtc_session|test_rtp_input|test_weak_network_manual|webrtc_service|test_webrtc_crypto|test_datagram_transport)$'
git diff --check
```

本轮地址迁移用例使用真实 STUN 消息构造与认证处理，媒体加密边界使用测试替身；单独的真实加密回归
不代表完成真实加密切网互通。尚未验收手机 4G/5G 切换，也未运行全量 CTest 或新增弱网压测。
构建仍有既有重复库链接等警告。

后续任务：恢复后的关键帧请求、路径恢复耗时统计、客户端有界重试及信令黑洞检测；异步发送历史仍需进一步
对齐真实 socket 完成回执。阅读顺序及接入契约见[服务端切网恢复框架](reference.md#信令恢复与-ice-restart)，
完整清单见当天变更说明（历史记录，报告文件未随仓库保留）。

本轮提交日志：

```text
feat(webrtc): 接入服务端切网路径状态与媒体控制恢复

- 增加 ICE 代次、路径修订号及可用路径生命周期通知
- 接通会话到媒体适配器的状态回调，重置旧路径反馈与带宽估计
- 按路径 epoch 作废旧异步发送，保留 TWCC 序号和媒体会话身份
- 补齐路径迁移回归、可选构建输入检查及核心框架文档

验证：Packetia 构建成功，6 组相关 CTest 通过；真实移动网络切换待验收。
```

## 2026-10-21：多编码转码与 RTP 弱网恢复

- 新增五种视频格式的软件适配、公共配置与接口、实现工厂；分开格式、具体库、码流表示和传输 PT。
- 接通完整编码帧输入、一次解码、多档缩放编码和独立输出路由，提供任务启停、状态、准入及过载恢复。
- 重构通用 RTP 排序队列，补齐 H264 组帧期限、媒体 worker 空闲检查和长断流前向重同步。
- 完成修改前后各 72 次本机弱网试验，保留 CSV、JSONL、缓存采样及环境；完整构建和 54 组 CTest 通过。
- 整理格式兼容规划、实际适配边界与验证数据；下一步优先打通转码输出的协议适配和播放链路。

详细清单及实测结论见2026-10-21 变更说明（历史记录，报告文件未随仓库保留）。

## 2026-10-14：HTTP 接入、API 路由与统一线程管理

- 将 HTTP 协议模块从 `network/http` 迁移到 `protocol/http`，独立构建为 `packetia_http`；
  规范文件与接口命名为 `HttpServer`、`IHttpServer`、`HttpServerOptions`、`HttpMessage`，
  同步更新引用和 CMake 依赖，公共字符串/HTTP 头校验工具移入 `utils`。
- 实现基于 libwebsockets 的 HTTP 监听、请求体分段接收、连接复用与异步回复，复用现有 I/O 调度器；
  增加请求/响应缓冲上限、连接限额和超时，工作线程的回复回到所属线程发送，关闭后迟到回复失效。
- 参照 ZLMediaKit 的接口注册方式提取 `ApiRouter`、`ApiResponse`，按方法与路径注册独立处理函数；
  启动时构建只读路由快照，统一 JSON 检查、错误响应、404/405 与异常处理，新增接口无需扩展集中判断分支。
- `AccountController` 注册账号、登录、当前用户查询、会话续期和退出共 5 个 HTTP 入口，
  受保护路由复用 Bearer 凭据检查；`ApiServer` 使用独立有界业务队列，超过准入限额返回 `503`。
  后续补齐 SQLite 存储、OpenSSL scrypt 与登录会话业务；服务和自助注册默认关闭，存储初始化失败时回滚启动。
- 新增 `ThreadManager` 和 `ManagedThread`，统一项目后台线程的创建、命名、状态登记、协作停止及回收；
  接入 I/O、媒体/Endpoint/转码/API/录制工作池、AI、混流和异步日志，主线程通过 `TrackCurrent` 登记。
  保留事件循环、按键分片的工作队列和专用循环各自的调度方式，由服务层按依赖顺序启停。
- 新增 `ServerApp::ThreadStatus()` 与 `ThreadRuntimeStatus`，汇总线程模型/状态/异常、命名池队列与丢弃统计、
  I/O 调度器等待任务数、执行数和拒绝数；`workers` 配置接管媒体、Endpoint、转码池的线程数和队列长度。
- 补齐线程启动失败的回滚及 I/O 异常退出清理，避免调用方等待已退出调度器的 `Invoke`；
  旧 `QueueManager` 改用有界命名池，增加 `Stop` 和 `TryPost`，支持排空和重新启动。
- 更新示例配置、HTTP 接口说明及线程管理文档，补充线程回收、并发停止、服务重启和框架回归测试；
  本轮 11 个相关 CTest 入口通过，主程序在 libwebsockets 开启/关闭两种配置下编译通过。

该历史阶段尚未实现 HTTP 房间管理/入房凭据签发；现已在独立控制入口接入。原生 HTTPS/CORS、线程运行时扩缩容和 WebRTC/TURN 会话分片仍未完成；
第三方编解码库内部线程通过各库接口管理。测试范围与复现命令见[本轮验证](reference.md#本轮验证)。

## 2026-10-13：房间 JWT 与参与者鉴权

- 按 LiveKit 的职责划分加入 API key provider、HS256 JWT 验证与 room grants 授权。
- 替换 WebRTC 共享 token，绑定可信用户身份与房间权限，授权后再创建媒体资源。
- 恢复校验 JWT 身份、原权限和随机恢复凭据，关闭旧独立 offer 的鉴权绕过入口。
- 迁移配置、签发工具及双浏览器示例，验证观众经 TURN 与双方重新鉴权恢复。

## 2026-10-12：集成发送侧带宽算法与修复 SRTP 重传

- 将参考 OU 时延检测、接收速率与 RemoteRateControl 码率算法适配到现有 TWCC 链路。
- 统一启动码率约束，补齐 RTT 传递、时间驱动的升码率、丢包观察及网络/路由重置。
- 区分实测接收速率和目标码率，保护低码率减法、100% 丢包及大码率计算边界。
- 允许发送端重新保护 NACK 缓存包，验证接收端仍拒绝迟到原包和重复重传。
- 补充 15 项带宽算法测试及弱网接入说明；当前闭环到达控制输出接口。

## 2026-10-10：网络边界、TLS 传输与参与者断线恢复

- 收紧网络对象的调度归属、TCP 启动顺序、缓冲上限及关闭流程，增加服务端 TLS 传输组件。
- TURN UDP 控制端口增加基础 STUN Binding 地址发现。
- 提取参与者身份、信令绑定代数、状态快照和媒体会话数据结构，明确服务与参与者的对象归属。
- 增加恢复窗口、随机恢复凭据和新信令绑定；ICE restart 保留媒体会话、DTLS/SRTP 与 SFU 转发关系。
- 房间示例增加自动重连及重新收集 ICE 候选，完成直接接入/强制 TURN 的双浏览器恢复验证。

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

## Version 0.3.0 – 2025-12-30
### Highlights

This release completes the RTSP-over-TCP interleaved media pipeline by
introducing explicit channel-to-track binding and a codec-aware RTP track
factory, enabling correct RTP/RTCP demultiplexing and per-track packet handling.

### Improvements

- Introduce RtpInterleaved channel map to dispatch interleaved RTP/RTCP packets
by negotiated TCP channel

- Bind RTP/RTCP interleaved channels to tracks during SETUP negotiation

- Add createTrack() factory to construct codec-specific RTP tracks

- Parse a=control:streamid=N safely via a dedicated ParseStreamId() helper

- Improve RTP debug visibility with optional per-packet header logging

- Generate compile_commands.json via CMake for accurate IDE tooling support

### Bug Fixes

- Fix incorrect use of track index as interleaved channel ID

- Fix RTP/RTCP packets being routed without validated SETUP binding

- Fix potential session/track mismatch during multi-track SETUP sequences

- Fix undefined reference caused by missing function definitions at link time

### Design Changes

- Clearly separate RTSP control plane (request/response parsing)
from media data plane (RTP/RTCP packet dispatch)

- RtpTrack lifetime is now managed via shared_ptr, while interleaved dispatch
uses weak_ptr to avoid ownership cycles

- TCP interleaved channels are treated as connection-scoped identifiers,
independent of media track indices


## Version 0.2.1 – 2026-01-01
### Highlights

- This release fixes RTSP RECORD failures caused by missing track registration during SETUP, and introduces an initial - - RTP-over-TCP interleaved processing pipeline that decouples network I/O from media processing.

### Improvements

- Register SDP tracks into MediaSession::tracks_ during SETUP to ensure RECORD operates on a fully initialized session

- Add MediaSession::BindRtpTrack(trackIdx, track_ptr) to associate trackIdx -> RtpTrack for deterministic lookup during - - RECORD and media processing

- Introduce an initial worker-based RTP input pipeline (PacketPool + WorkerPool) to offload heavy RTP processing from the connection read thread

- Add structured logging for SETUP track matching (control/codec/pt/clock/trackIdx) and for RECORD session/track validation

### Bug Fixes

- Fix RTSP RECORD returning no response when MediaSession::tracks_ is empty (root cause: SETUP did not register tracks)

- Fix incorrect/misleading logs in RECORD handler (e.g., printing “SETUP request” in RECORD path)

## Version 0.3.0 – 2026-01-02
### Highlights

- This release introduces a reusable real-time packet delivery framework for RTP/RTCP processing. It adds a fixed-capacity PacketPool for deterministic memory management and a dedicated SPSC RtpRingBuffer (with RTCP priority) to decouple I/O from media processing, improving stability under load.

### Improvements

- Add PacketPool (object/memory pool) to provide fixed-capacity packet buffers and avoid frequent heap allocations in the RTP data path

- Add RtpRingBuffer built on SPSC rings, supporting separate RTP/RTCP queues with RTCP-first dequeue policy

- Refactor interleaved RTP ingestion to a “copy + enqueue” model, simplifying the read thread responsibilities and reducing lifetime hazards

- Introduce queue/pool observability hooks (queue depth, dropped/exhausted counters) to facilitate load testing and tuning

### Bug Fixes

- Fix potential lifetime issues when dispatching interleaved RTP/RTCP across threads by avoiding raw pointer ownership leaks and centralizing buffer release

- Resolve const-correct locking issue in PacketPool by making internal mutex mutable (enables thread-safe const observers such as size()/stats())


## Version 0.3.1 – 2026-01-06
### refactor(network): fix connection lifetime issues and introduce factory-based initialization

- Introduce two-phase initialization for TcpConnection (construct + Start)
- Move channel registration and event enabling out of constructors
- Add factory method RtspConnection::Create to enforce correct init order
- Replace raw `this` captures in callbacks with weak_ptr to prevent UAF
- Eliminate bad_weak_ptr caused by shared_from_this in constructors
- Unify RTSP connection creation through server OnConnect factory path
- Improve disconnect handling to avoid delayed callback accessing destroyed server


## Version 0.3.2 – 2026-01-07


### Architecture Changes

- Introduce a generic ShardedWorkerPool with key-based sharding to guarantee
per-key ordering while allowing parallel execution across workers.

- Add a pluggable IJobHandler interface, enabling different subsystems
(RTP, RTCP, media processing, future non-RTSP tasks) to reuse the same worker pool.

- Refactor RTP processing into a clear producer → dispatcher → consumer model:

- IO thread: framing and minimal copy only

- Worker threads: RTP/RTCP consumption and track-level processing

- Decouple RTP consumption logic from RTSP, making the worker service reusable
by other modules.

### RTP / RTSP Improvements

- Implement a clean RTSP over TCP interleaved handling model:

- Strict $ <channel> <length> <payload> framing

- Robust handling of sticky packets and fragmented frames

- Add explicit channel → track binding established during RTSP SETUP.

- Ensure RTP/RTCP packets are never parsed by the RTSP control parser.

- Move all heavy RTP processing out of IO threads to worker threads.

### Worker & Scheduling

- Add queue depth limits and drop policies (DropHead / DropTail) to protect
the system under load.

- Guarantee in-order processing for the same track_id via consistent sharding.

- Provide safe shutdown semantics with optional queue draining.

- Improve statistics collection for enqueue, dequeue, drops, and max queue depth.

### Memory & Stability

- Centralize packet lifetime management via PacketPool.

- Ensure all dropped or unbound jobs properly release packet memory.

- Eliminate potential memory leaks when queues overflow or tracks are unbound.

- Improve defensive checks for invalid channels and oversized RTP packets.

## Version 0.3.3 – 2026-01-11
### fix(workerpool): fix PacketPool leak by restoring job-based release chain

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


## Version 0.3.3 - 2026-01-13
### feature: implement RTSP TCP interleaved demux and RTP worker dispatch

- Add RTSP interleaved ($) frame parsing in RtspConnection
- Bind interleaved channel to RtpTrack during SETUP (RTP/RTCP)
- Deliver interleaved RTP/RTCP payloads to media WorkerPool
- Use track-based key for per-track serial processing
- Integrate PacketPool for RTP payload buffering

tips:I'm tired today, going to rest and then continue studying BBR and Gerrit
This establishes the complete TCP → channel → track → worker RTP pipeline.


## Version 0.3.4 - 2026-01-14
### fix: optimize RTP track map locking and reduce log noise

- Replace std::mutex with std::shared_mutex for RtpJobHandler track map
  to improve concurrency under high RTP load (read-heavy scenario)
- Use shared_lock for track lookup and unique_lock for track insert/erase
- Downgrade excessive ERROR logs to WARN for normal track teardown and race cases
- Improve log context to include key and payload length for easier troubleshooting

tips: I’m just figuring out how to complete my own RTP pipeline, mainly by following ZLMediaKit’s design.
This change avoids unnecessary lock contention in RTP worker threads
and prevents log flooding during track lifecycle transitions.

## Version 0.3.5 - 2026-01- 15
### feat(rtsp/rtp): introduce RTP wire header parsing and packet reorder pipeline

- Add RtpWireHeader to correctly parse RTP wire-format headers (RFC3550)
- Separate RTP wire header from logical RtpHeader to avoid ABI/layout bugs
- Refactor RtpVideoTracker::inputRtp to parse PT/SSRC/SEQ/Timestamp from wire header
- Integrate EnhancedPacketSortor to handle RTP reordering and wrap-around
- Prepare hooks for PT/SSRC locking and NTP-based timestamp conversion
- Lay groundwork for TCP-interleaved and UDP unified RTP input pipeline

tips: I'm just learn how to work jitter buffer

This change fixes incorrect RTP header parsing, prevents random seq/pt/ssrc
misinterpretation, and enables stable real-time packet reordering.

## Version 0.3.6 - 2026-01- 16
### refactor(rtsp): rework RTP over TCP interleaved dispatch and job handling

- Introduce channel-to-track binding for RTP/RTCP interleaved streams
- Pass weak_ptr<RtpTrack> through WorkJob instead of relying on handler-side maps
- Clarify ownership and lifetime between interleaved parser and worker threads
- Prepare worker path for safe RTP packet processing

Note:
Further investigation is required for packet lifetime and use-after-free issues.

## Version 0.3.7 - 2026 - 01 - 19
### fix: resolve RTP packet lifetime issues in worker pool

- Fix heap-use-after-free caused by incorrect ownership of RTP packet memory
- Ensure Packet lifetime is managed consistently across worker threads
- Clarify WorkJob payload semantics (Packet* vs raw buffer)
- Avoid double-free by unifying packet release responsibility
- Improve robustness of RtpJobHandler::handle under concurrent execution

## Version 0.3.8 - 2026-01-20
### rtp: wire RTP parsing into worker → track pipeline
- Add RTP raw packet handling in RtpJobHandler
- Parse and validate RTP headers before track processing
- Construct RtpPacket from raw bytes and hand off to RtpTrack
- Prepare track-level pipeline for ordered RTP processing
- Lay groundwork for jitter buffer and depacketizer integration


## Version 0.3.9 - 2026-01-21
### refactor: add RTCP packet type enums and feedback definitions
- Define RTCP packet type enums (SR/RR/SDES/BYE/APP/RTPFB/PSFB/XR)
- Add SDES item type definitions per RFC3550
- Add RTPFB/PSFB feedback type enums (NACK/PLI/FIR/REMB, etc.)
- Use strongly-typed enum class with protocol-aligned values
- Prepare groundwork for RTCP parsing and statistics handling


## Version 0.4.0 - 2026-01-23
### refactor: rework RTP job payload model and fix abort caused by invalid length'
- Replace void* payload with std::variant
- Use shared_ptr<Packet> for safe cross-thread ownership
- Remove unsafe static_cast paths
- Fix len=0 propagation to inputRtp()

## Version 0.4.1 - 2026-01-27
### feat(rtp): implement RTP parsing, jitter buffering and H264 depacketization

- Improve RTP header parsing and validation
- Integrate jitter buffer for packet reordering
- Implement ordered RTP callback
- Support H264 Single / STAP-A / FU-A depacketization
- Output Annex-B formatted frames

## DEBUG - 2026-01-28
### debug: verify RTP pipeline end-to-end and locate PacketPool exhaustion path

- Verified full RTP flow from worker dispatch to RtpVideoTracker::onRtpSorted
- Confirmed callback and sorting stages are correctly triggered
- Identified potential lifetime and caching issues causing PacketPool exhaustion

## Docs - 2026-01-31
### docs: add RTCP XR (RFC 3611) protocol and SDP signaling notes
- Document RTCP XR common header (PT=207) and packet structure
- Describe the XR report block model and usage scenarios
- Clarify sequence-number–based reporting for Duplicate RLE and Packet Receipt Times blocks
- Explain RTT measurement using Receiver Reference Time (BT=4) and DLRR (BT=5)
- Add SDP a=rtcp-xr signaling rules, distinguishing unilateral and collaborative parameters
- Clarify Offer/Answer behavior and bandwidth considerations for XR usage

This change only updates protocol documentation and design notes, without affecting
existing RTP/RTCP data path logic.

## ROOM:20260204
### feat: introduce ClientSession for per-client send handling

- Isolate per-client RTP send queue and state into ClientSession
- Prepare MediaSession for SFU-style multi-subscriber forwarding

## RoomVersion:0.0.1: 2026-02-05
### feat: add basic SFU room

- Introduce Room module to manage participants and provide basic conference routing.
- Add participant join/leave management and broadcast forwarding logic.
- Each incoming RTP packet is forwarded to all other participants (N-1 fanout).
- Prepare foundation for future subscribe-based routing / simulcast / SVC.

## 2026-02-06
### feat(rtsp/rtp): add RTP packet sorting logs and improve debug tracing
- Add detailed debug logs for RTP sorting pipeline (EnhancedPacketSortor)
- Print seq/next_seq/buffer state to verify jitter-buffer reorder behavior
- Add gdb breakpoint tracing points for emit() / inputRtp() call path
- Improve packet dump helper to validate RTP header correctness
- Facilitate troubleshooting for RTP packet payload/ts/seq parsing issues

## Version 0.4.2 - 2026-02-08
### feat(core): add signal-based subscription framework for stream dispatch
- Add ISubscription / ISignal interfaces for callback subscription model
- Implement SignalCOW with copy-on-write snapshots; emit does not hold the subscription writer mutex
- Provide subscribe/cancel mechanism to manage listener lifecycle
- Introduce SourceBase<T> abstraction to expose publish/subscribe pattern for stream modules
- Prepare foundation for RTP/frame fan-out and modular pipeline extension

## Version 0.4.3 -  2026-02-09
### Add Depacketizer module and start H264 RTP frame reassembly

- Introduced a new Depacketizer module and defined a unified interface (input() / hasFrame() / popFrame()) for frame-level reconstruction based on sorted RTP packets.
- Added initial H264Depacketizer class skeleton, preparing the architecture for future codec extensions (H265/VP8, etc.).
- Completed RTP packet metadata filling after parsing, including ts/marker/version/padding/extension/cc/hdr_len/payload_off/payload_len, ensuring downstream depacketization can rely on correct header/payload boundaries.
- Improved RTP header parsing to correctly handle CSRC and header extensions, with additional validation for padding scenarios.
- Added debug logs for RtpSorted output to verify sequence continuity and payload size variations, confirming correct behavior before implementing FU-A/STAP-A reassembly logic.

## Version 0.4.4 - 2026-02-14
### Introduce RTCP module architecture and prepare RTP/RTCP interleaved processing

- Added initial RTCP module framework following a WebRTC-style interface design, including IRtcpReceiver, IRtcpSender, and IRtcpObserver for clean protocol/business separation.
- Implemented RtcpReceiverImpl skeleton with core entry points (OnRtcpPacket, SetObserver, SetLocalSsrc, SetRemoteSsrc) to prepare for compound RTCP parsing.
- Confirmed RTSP interleaved transport behavior where RTP and RTCP share the same TCP socket, and identified channel-based demux logic (interleaved=RTP-RTCP mapping).
- Prepared media pipeline integration points for routing interleaved RTCP payloads into the RTCP receiver, enabling future support for RR/NACK/PLI feedback handling.
- Reviewed RtspConnection initialization flow and verified worker pool (media) and packet pool integration to support upcoming RTCP parsing and retransmission work.


## Version 0.4.5- 2026-02-15
### Title: Add RTCP receiver integration and fix build/link issues in RTP track
- Introduced RTCP handling interface (inputRtcp) in RtpTrack and implemented RTCP callback mechanism via IRtcpObserver.
- Integrated RtcpReceiverImpl into RtpVideoTracker to support parsing RTCP packets (RR/NACK/PLI events reserved).
- Fixed namespace and constructor signature mismatch for RtcpReceiverImpl (rtcpx::IRtcpObserver*) to resolve undefined reference issues.
- Updated CMake build linkage to ensure RTCP implementation is correctly compiled and linked for unit tests.
Improved RTSP/RTP module structure preparing for future RTCP feedback processing and keyframe request support.

## Version 0.4.6 - 2026-02-16
### Refactor TCP stack by introducing a Session/Observer based architecture.
- Added generic ICodec<Msg> interface to support protocol-level decoding/encoding (SIP/RTMP/RTSP, etc.).
- Implemented ObserverList to broadcast decoded messages to multiple business modules via ISessionObserver.
- Introduced IConnectionObserver and improved connection-to-session callback flow for byte-level events.
- Updated TcpConnection to expose bytes callbacks so TcpSession can take ownership of protocol parsing logic.
- Prepared the framework for multi-protocol session management with clean transport/protocol separation.

tips: Today is Chinese New Year. I stayed in my rented apartment and spent the day coding.


## Commit Message (2026-02-17)
Refactored multi-protocol TCP session architecture by introducing ProtocolDetector and ProtocolDetectorSession, enabling dynamic protocol detection and seamless promotion to protocol-specific sessions (SIP/RTSP), while fixing include dependency and compilation issues in SIP parser integration.

## Version 0.4.7 - 2026-02-18
### Refactored protocol detection and RTSP parser integration by introducing RtspProtocolParser based on ProtocolParser
- Added RtspProtocolParser implementation based on ProtocolParser to support RTSP protocol detection (including $ interleaved framing).
- Refactored parser declarations/definitions and fixed ParseResult scope + missing return issues to resolve compilation errors.
- Updated build integration and linkage to eliminate duplicate Parse declarations and vtable undefined reference errors.

## Version 0.4.8 - 2026-2-20
### Refactor: Introduce protocol factory and session promotion mechanism
- Added ISessionFactory abstraction and integrated factory injection into TcpServer, enabling protocol-level session creation without coupling transport layer to specific protocol implementations.
- Implemented dynamic session promotion in ProtocolDetectorSession: upon successful protocol detection, create concrete session (e.g., RTSP) via factory and replace current session mapping.
- Optimized detection flow to ensure newly promoted session immediately processes existing buffer data, preventing first-packet loss during protocol switch.
- Decoupled RtspSession from RtspServer to reduce strong dependencies and improve modularity of protocol layer.

## Version 0.4.9 - 2026-2-22
### feat(network): introduce UDP server abstraction integrated with EventLoop
- Added UdpSocket encapsulation for non-blocking UDP operations (create/bind/recvfrom/sendto).
- Implemented UdpServer with Channel-based integration into existing Reactor (EventLoop + EpollTaskScheduler).
- Introduced IUdpHandler interface for decoupled datagram processing.
- Enabled TCP and UDP servers to coexist under the same EventLoop.
- Prepared foundation for future RTP/RTCP/ICE integration.

## Version 0.5.0 - 2026-2-23
### feat(media): introduce UDP transport skeleton and session demux layer
- Add MediaEngine as transport entry container to manage UdpServer lifecycle
- Implement UdpMuxHandler to demultiplex STUN / RTP / RTCP / DTLS packets
- Introduce UdpSession abstraction to encapsulate per-peer state
- Define initial session lookup/create mechanism (src-based mapping, preparatory for ICE-lite integration)
- Refactor ownership model: UdpServer uses unique/shared ownership at engine level; sessions hold non-owning reference
- Prepare groundwork for future ICE-lite + RTP integration

## Version 0.5.1 - 2026-2-24
### udp: add peer-based session routing and protocol demux
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

## 2026-09-14 — 完善录像生命周期并接通发送侧 GCC 反馈链路

### 本次完成

- 拆分 `RecordingSession`、`RecordingInstance` 和 `RecordingSegment`，明确录像会话、执行实例与文件分段的职责。
- 引入 `IRecorder` 和录像事件回调，补充状态流转、停止原因及失败通知。
- 录像先写入临时文件，成功收尾后重命名为正式 MP4 文件。
- 统一编码帧订阅接口为 `SubmitFrame`，同步适配录像和 AI 模块。
- 新增 Transport 级 `SendSideController`，统一管理发送历史与 GCC。
- 音视频及重传共享 TWCC 序号，发送成功后记录包信息。
- 接通 TWCC/RR 反馈处理与目标码率、Pacer 配置输出回调。
- 补充序号回绕匹配、重复反馈过滤和断线状态重置。
- 保留 TWCC 带宽估计，避免 RTT 更新额外改变目标码率。
- 拆出 `media_quality` 库，明确发送轨与控制器的链接依赖。
- 新增 8 个发送侧链路测试及[接入文档](reference.md#弱网与带宽控制)。

### 验证情况

- 发送侧与原有弱网测试共 16 项，独立构建运行通过。
- 完整服务及录像改动尚未完成本轮联调。

### 后续工作

- 当前 GCC 链路接至控制输出接口，继续接入实际 Pacer 发包和编码器调码率。
- 实现 TWCC 超时后的 RR 回退策略。

## 2026-09-23 — 重构 SFU 端点，支持多轨发布与订阅

### 本次完成

- 将 SDP 媒体描述统一转换为现有 RTP Track 参数，现由 `Rtsp/Rtp` 中的 `RtpStreamContext` 和 `SdpTrackBinding` 承接。
- 支持单个 `SfuEndpoint` 管理多条发布轨道，按 MID、SSRC 和轨道提示分流。
- 将接收 Track 归属发布端点、发送 Track 归属订阅端点，通过工作线程投递完成跨端点转发。
- 接通房间发布、订阅、取消订阅、取消发布和参与者退出的媒体生命周期。
- 支持下游 RTP 参数及 MID/TWCC 改写，接通 NACK 缓存重传和 PLI 反馈。
- 调整 RTSP 多轨共享端点，完善 SETUP 失败回滚和 TEARDOWN 清理。
- 新增 `WebRtcMediaTransport`，适配 WebRTC 会话与统一媒体收发接口。
- 完善订阅失效、排队旧包丢弃及端点停止后的资源清理。
- 补充多轨转发、房间订阅、SDP 绑定、WebRTC 传输适配和 RTSP 多轨集成测试。

### 验证情况

- 主程序编译通过，新增 10 项测试全部通过，RTSP、SDP 和录制相关回归通过。
- 完整回归仍有两项已知失败：WebRTC STUN 提名、超大 RTP 包计数。

### 当前限制

- WebRTC 应用层信令组装及实际 DTLS/SRTP 后端仍待接入，尚未完成真实浏览器链路验证。
- 暂不支持 RTX 和 simulcast 层选择；每条订阅固定转发一个源编码的 SSRC。
- 房间接入仍需应用层绑定参与者端点，并根据下游协商结果配置订阅参数。

## 2026-09-24 — 引入公共状态机并接入 ICE 与 RTSP 会话

### 本次完成

- 新增 `StateMachine` 和 `StateController`，支持强类型转换表、条件判断、动作回调和绑定业务上下文。
- 明确非法事件、重复规则、递归派发和异常处理语义。
- 将 ICE 生命周期改为显式状态，接通存活超时与 WebRTC 会话关闭，阻止关闭后的连接被迟到包激活。
- 修复 STUN MESSAGE-INTEGRITY 计算范围错误，防止认证失败的请求污染远端凭据及未认证属性触发提名。
- 将 RTSP 推流流程接入状态机，保留多轨 SETUP 和失败回滚，成功处理请求后才提交状态转换。
- 校验 RECORD/TEARDOWN 的会话归属，支持重复 RECORD、TEARDOWN 后复用 TCP 连接及断线清理。
- 未实现的 DESCRIBE、PLAY、PAUSE 明确返回 501。
- 补充公共状态机、ICE 和 RTSP 生命周期测试；相关实现见 [IceAgent](../protocol/ice/IceAgent.cpp) 及 [RtspSession](../Rtsp/RtspSession.cpp)。

### 验证情况

- 主程序编译通过，公共状态机、ICE、WebRTC 会话、传输及媒体适配相关测试通过。
- RTSP TCP/UDP、多轨 SETUP、失败回滚、错误请求顺序、会话重建和断线清理测试通过。
- 上一条记录中的 STUN 提名失败已修复；超大 RTP 包计数问题不在本次修改范围内。

### 当前限制

- 应用需在会话事件循环周期调用 `WebRtcSession::Tick`，确保无流量时也能及时检测 ICE 超时。
- ICE 建立与存活超时仍共用配置；会话级 ICE restart、完整主动检查及真实浏览器互通仍待完善。
- RTMP、AI 和公共服务生命周期尚未迁移至状态机组件。

SFU 信令模块划分、线程与双向通道、失败回滚和测试记录见 [重构说明](reference.md#媒体层结构与命名)。

## 2026-10-27 — 统一媒体参数分层、RTSP 鉴权与房间轨道事件

### 本次完成

**RTSP 复用公共鉴权与工具函数**

- 扩展 `AuthService` 的请求上下文，RTSP 使用 Digest 验证凭据，WebRTC 保留 JWT 路径；认证结果继续交给各自的访问策略判断权限。
- 新增 `RtspDigestAuthenticator`、`StreamAccessPolicy` 和 `RtspAuthOptions`，支持连接内 nonce 到期、重放检查和精确流路径权限。
- RTSP 在受保护命令入口重新认证，配置增加 `rtsp_auth`，默认关闭；提供 `scripts/make_rtsp_auth.py` 生成凭据配置。
- 统一 `AuthService.h` 等项目头文件的短名称引用，由 CMake 提供搜索路径。
- 将 `FindCrlfCrlf`、正文长度解析移入 `utils/HttpUtil.h`，将 `Trim`、`SplitOnce`、`ToLower` 等移入 `utils/StringUtil.h`，移除 RTSP 内重复实现。

**媒体流管理与参数隔离**

- 通过 `MediaStream`、`MediaStreamRegistry` 管理 RTSP 流注册、流内轨道及轨道到 Endpoint 的绑定；按对象身份移除注册，避免旧对象清理误删替代流。
- `RtspMediaSession` 保留 RTSP control、TCP channel 和接入生命周期职责；补齐重复发布拒绝、失败回滚、断连清理及 TEARDOWN 后下一次发布的资源边界。
- SFU 端点通过通用编码帧发布接口对接帧路由，移除不再使用的 `Rtsp/Media.h`、`MediaSource.h`。
- 新增 `media/types/MediaTypes.h`、`CodecNames.h`，统一媒体类型、编码身份、流向及编码名称别名，清理重复的基础枚举。
- 业务轨道元信息统一为 `TrackPublicationInfo`；`TrackPublication` 加锁读取并返回自有值，`TrackPublicationFwd.h` 仅保留前置声明及共享句柄。
- `MediaTrackDescription` 仅保存通用轨道 ID、媒体类型、编码及音频采样率/声道，不再混入 PT、RTP 时钟和头扩展编号。
- `media/rtp` 拆分纯数据 `RtpStreamContext`、候选选择/通用描述转换 `RtpTrackBinding` 和 SDP 适配 `SdpTrackBinding`。
- 新增 `Rtsp/RtpTrackAdapter`，将选中的 RTP 参数转换为旧接收器所需的 `::TrackInfo`，将兼容代码留在协议适配层。
- RTSP 的 SDP 一次解析为只读 RTP 快照，只保存选中 payload 的索引；通用轨道和旧接收器参数均从同一份选择派生，避免重复参数表漂移。
- 拆出 `IMediaStatsSink`，统一统计公共类型及 `MetricsCalculator` 命名空间，将统计实现构建为独立目标。
- CMake 分离 `packetia_media_types`、`packetia_media_core`、`packetia_media_rtp`、`packetia_sdp_media_adapter`、`packetia_media_stats`，同步更新录像、转码及测试依赖。

**EventBus 接通房间轨道通知**

- 每个 `Room` 拥有独立 Bus，新增 `TrackPublishedEvent`、`TrackUnpublishedEvent`；成功发布、取消发布、发布者离房和房间关闭均发布已提交的变化。
- 事件携带元信息值快照、发布实例 `publication_id` 和变更 `revision`；两种编号在当前房间实例内有效，同名重新发布获得新的发布实例编号。
- Bus 回调在房间锁外同步执行；订阅句柄可晚于 Bus 销毁，退订使快照中的槽失效，回调异常被隔离并返回失败计数。
- 已通过 active 检查的回调仍可能在退订后执行，因此信令订阅者只捕获版本槽的弱引用，不捕获房间、连接或 handler。
- `RoomSignalHandler` 在 owner 的 Tick 中合并变更，经现有有界响应通道发送 `tracks_changed` 完整列表；同一版本不重复通知，初始响应和 Answer 先入队。
- 通知检查房间范围与 ListTracks 权限，恢复期间不发给旧连接；恢复成功后向新连接补发当前快照，离房与停服解除订阅。
- 示例页面将主动通知与请求响应分流，收到通知后更新可选发布者列表，避免把通知误交给等待 Answer 的请求。

### 当前调用链与线程归属

```text
RTSP ANNOUNCE SDP
  → SdpTrackBinding：生成只读 RTP 参数快照
  → RtpTrackBinding：选择候选并生成通用轨道描述
  → MediaStream / MediaStreamRegistry：登记流与轨道
  → RtpTrackAdapter：生成旧接收器参数

WebRTC 发布请求
  → Gateway 解码 → 请求通道 → RoomSignalHandler
  → WebRtcSession 协商 → Room::PublishTrack
  → TrackPublishedEvent → Bus 回调更新版本槽
  → 返回 Answer → 响应通道 → Gateway 串行发送
  → 后续 Tick → tracks_changed → 同一响应通道 → 客户端更新列表
```

- RTSP 接入状态仍由连接所属 EventLoop 串行处理；媒体包继续交给现有 MediaWorker。
- WebRTC 房间状态与通知生成由现有服务 owner 执行器处理，WebSocket 写入仍经过连接执行器和统一出口。
- Bus 本身不切换线程、不排队重放事件；异步消费者必须复制事件数据并投递到所属执行器。
- 房间和 Endpoint 的直接调用继续保证路由清理顺序，Bus 传播结果，不依赖订阅者顺序完成必需清理。

现有目录职责见[RTP 参数与 SDP 适配](reference.md#媒体层结构与命名)和[房间信令与轨道事件](reference.md#媒体层结构与命名)。

### 验证情况

- `Packetia`、`PacketiaControl`、`PacketiaSfu` 构建通过；链接器仍有重复库警告。
- 媒体隔离阶段：`server_app`、`media_parameters`、`test_rtp_input`、`test_udp_binding`、`rtsp_auth`、`webrtc_service` 共 6 组 CTest 通过。
- 录像独立测试 3/3 通过，转码独立测试 3/3 通过；这两项为媒体隔离阶段结果，Bus 接入后未重复运行。
- Bus 接入后：`server_app`、`media_parameters`、`test_rtp_input`、`webrtc_service`、`signal_channel` 共 5 组 CTest 通过。
- 新增 Bus 句柄析构、退订快照过滤、回调异常隔离、弱引用及捕获对象析构测试；新增房间发布/移除、同名重发、离房和重复关闭测试。
- 真实 WebSocket 集成测试覆盖发布主动通知、跨房间隔离、重连快照补发和恢复超时后的移除通知；补充通知合并及版本不变时不重复发送的断言后，`webrtc_service` 再次通过。
- 示例页面通过 `node --check`，`git diff --check` 通过；本次未重新运行浏览器媒体互通、公网、容量或完整回归测试。

### 当前限制与后续工作

- WebRTC 房间轨道仍由 Room/Participant 管理，尚未全部统一到 `MediaStreamRegistry`；业务字符串轨道 ID 与 core 流内数值 ID 仍是不同作用域。
- 完整 WebRTC SDP 保留在连接会话的协商层；`RtpStreamContext` 是参数子集，不存完整 SDP。RTSP 当前保留 RTP 快照和 control 映射，不长期保存完整 SDP 描述。
- `tracks_changed` 是最新完整列表，多次变化可能合并；客户端必须与请求响应分流。通知不代表已开始收到媒体，也不会自动创建订阅或触发动态重协商。
- 轨道级 Open/Closing/Closed、订阅意图与重试、动态权限、静音转发控制、通用 MediaStream/RTSP 流状态事件尚未接入。
- `MediaStatsCore` 的 RTP/RTCP/帧事件处理仍为空实现，接口隔离不等于已经接通实时统计。
- 本次未新增 RTMP/SRT 接入或跨协议输出，也未引入 RTX、simulcast 层选择或新的拥塞控制算法。

## 2026-10-28 — 将 RTP 参数与 SDP 适配移回协议目录

- 将 `RtpStreamContext`、`RtpTrackBinding`、`SdpTrackBinding` 共 5 个文件从 `media/rtp` 迁入现有 `Rtsp/Rtp`。
- 在 `Rtsp/Rtp/CMakeLists.txt` 定义 `packetia_rtp_binding`、`packetia_sdp_rtp_adapter`，替代前一日的 `packetia_media_rtp`、`packetia_sdp_media_adapter`。
- 同步调整头文件搜索目录、媒体端点及参数测试的目标依赖；保留短名称 include 和原有调用接口。
- 通用轨道仍位于 `media/track`，RTP/SDP 转换供 RTSP 与 WebRTC 复用；共享传输抽象继续位于 `media/transport`。
- 仅整理目录和构建归属，不修改协商参数选择、轨道 ID 或媒体处理算法。
- 验证：`Packetia`、`PacketiaControl`、`PacketiaSfu` 构建通过；`media_parameters`、`test_rtp_input`、`test_udp_binding`、`webrtc_service` 共 4 组 CTest 通过。纯 RTP 选择实现仅使用协议目录及通用媒体头文件即可独立语法编译。

## 2026-10-28 — 留在会议中停止媒体并重新发起

- 新增 `unpublish` / `unsubscribe` 信令，停止整个当前发布或订阅媒体会话，保留参与者、房间、信令连接和恢复凭据；可在同一次入房后重新发布或订阅。
- 请求必须携带媒体 Answer 中的正整数 `session_id`，并通过对应权限、当前编号和媒体角色检查。重复停止在没有当前媒体时返回 `changed:false`；已有新媒体时拒绝旧编号，避免误清理。
- 复用 `RemoveMediaSession`，在服务 owner 执行器回收轨道/路由、Participant 媒体绑定、RTC、传输、UDP mux 登记及 Endpoint。停止发布通过现有 Bus 推送 `tracks_changed`；响应沿原通道串行发送。
- 示例页面增加 Stop publishing / Stop watching，确认后关闭 PeerConnection，发布端释放摄像头与麦克风。补充无媒体恢复及旧连接候选/统计回调的过滤。
- 真实 WebSocket 集成测试覆盖权限与参数拒绝、角色检查、重复停止、旧编号保护、资源及下游路由回收、移除/重发通知、重新订阅和停止媒体后的身份恢复。
- 验证：`Packetia`、`PacketiaControl`、`PacketiaSfu` 构建通过；`webrtc_service`、`signal_channel` 两组 CTest 通过；页面及浏览器回归脚本的 `node --check`、`git diff --check` 通过。
- 浏览器回归脚本已补充停止/重建流程，但未运行媒体互通：当前源码缺少可选的 `server/examples/WebRtcRoomDemo.cpp`，无法重建对应演示服务。未验证公网和容量。
- 当前仍为每连接一个媒体会话，操作作用于整会话；源轨道重发后，观看端需显式停止旧订阅并重新订阅，不自动恢复路由。协议详情见[房间信令说明](reference.md#媒体层结构与命名)。

## 2026-10-29 — WebRTC 房间固定 owner

- 根据房间 ID 固定分配执行器，同一房间的成员、协商、发布、订阅和清理串行处理；不同 owner 的房间可独立推进。
- 网络执行器保留 WebSocket 和一个共享 UDP 端口；信令、通知和数据报通过异步通道跨 owner 转发。
- 异步请求完成前保留预算并暂停下一条请求，保证同连接离房后跨 owner 入房的顺序；成员和媒体会话额度仍按全服务限制。
- 增加 UDP 队列与旧路由代次过滤；执行器拒收断连清理时由 Tick 重试，停服按接入、房间、网络顺序回收。
- 原 `Packetia` 入口配置 `server.io_threads: 3` 可使用 1 个网络执行器和 2 个房间 owner；独立 `PacketiaSfu` 入口当前仍为单 I/O 线程。
- `Packetia`、`PacketiaControl`、`PacketiaSfu` 构建通过；`server_app`、`test_datagram_transport`、`webrtc_service`、`signal_channel` 共 4 组 CTest 全部通过，`git diff --check` 通过。
- 未重新验证浏览器媒体、公网或容量；线程模型、实际文件和调用链见[房间 owner 说明](reference.md#webrtc-房间-owner)。

## 2026-10-29 — 分离通用队列与 UDP 发送职责

- 将通用排队、预算、门控和异步完成机制提取到 `utils/BoundedChannel.h`；`SignalChannel` 保留信令接口与首响通知顺序。
- 新增 `network/transport/UdpSendQueue`，队列保存目标地址、独立缓冲区与传输弱引用；网络执行器负责实际发送。
- `WebRtcService::Start` 只组装发送队列和 UDP mux；UDP 入站任务也改用公共队列，移除对信令队列的依赖。
- UDP 默认 4 MiB / 4096 条限制集中定义，原有预算、关闭检查及排队顺序保持；新增真实 UDP 缓冲区、背压和取消回归。
- 验证：三个运行入口与相关测试构建成功，`server_app`、`test_datagram_transport`、`webrtc_service`、`signal_channel` 共 4 组 CTest 通过；公共队列独立语法编译及 `git diff --check` 通过。

## 2026-11-04 — 下行订阅带宽策略、指标平滑与接收评估器生命周期

### 本次完成

- 新增 `DownlinkBandwidthAllocator`，将每条 Transport 的带宽估计转为订阅准入策略：默认使用目标码率的 85%，优先保留音频及豁免轨道，再按订阅优先级分配视频预算；不同观众独立分配。
- 支持通过 `SetSubscriptionBandwidthPolicy` 设置订阅优先级、需求码率提示及自动暂停豁免，通过 `GetSubscriptionBandwidthStatus` 查询分配状态。优先级属于观众的一条订阅关系。
- 发布轨道共享 `SourceBitrateEstimator`，根据源 RTP 字节速率估算需求；某个观众暂停时仍保留源需求，避免按暂停后的发送速率错误分配。
- 带宽不足持续 300 ms 才暂停视频，恢复要求额外 20% 余量并持续满足 1000 ms。带宽状态独立维护 `Forwarding`、`Paused`、`WaitingKeyFrame`，暂停期间保留订阅和发送器，阻止媒体及缓存重传。
- 新增 `VideoResumeBuffer`，恢复时等待完整 H264/H265 IDR，支持分片、乱序和序号回绕；接通发送序号连续化、padding 探测及媒体 Worker 上的分配与恢复调度。
- 拆分 `DownlinkTrackManager`、`ReceiveFeedbackHandler`、`ReceiveQualityMonitor` 和 `EncodedFrameDispatcher`，分别管理下行订阅、接收反馈、按 SSRC 的质量状态和编码帧投递。
- 将通用 `EwmaFilter` 接入实际指标采样：上行按 SSRC、下行按 Transport 独立保存统计；RTT、丢包率、jitter 和码率按实际采样间隔平滑，每项指标独立过期，缺失样本不刷新其他指标。
- 接收轨道在 `GetOrCreateReceiverTrack()` 成功创建时，通过 `RegisterSource(ssrc)` 同步创建评估器，覆盖 RTP 先到与 SR 先到；初始为 `Unknown`，注册不产生统计样本或发送 RTCP。
- 反馈只更新已注册评估器，重复注册保留历史；轨道删除调用 `Remove(ssrc)`，端点停止调用 `Clear()`，迟到反馈不会重建已删除状态。新增 `GetReceiveQualitySnapshot` 查询接口，并补齐初始化异常回滚。
- 根目录 `.gitignore` 增加 `/test/`，忽略根目录本地测试目录，保留 `Rtsp/Test/`、`media/tests/` 和 `media/quality/tests/` 中的项目测试源码。

实现与接口示例见[弱网与带宽控制](reference.md#弱网与带宽控制)，主要代码位于
`media/quality/DownlinkBandwidthAllocator.*`、`media/quality/NetworkStatsCollector.*`、
`media/quality/ReceiveQualityMonitor.*`、`media/endpoint/` 和 `Rtsp/Rtp/VideoResumeBuffer.*`。

### 验证情况

- `Packetia` 及相关测试目标构建通过。
- 新增订阅预算、视频暂停恢复、指标平滑与采样测试；另新增 4 项接收评估器生命周期测试，覆盖重复注册、无样本初始状态、创建失败、SR 先到、删除及停止清理。
- 最终回归中，`test_weak_network_manual`、`test_rtp_input`、`test_udp_binding`、`media_core`、`test_webrtc_crypto`、`webrtc_service` 共 6 组 CTest 全部通过。

### 当前边界

- 自动分配只使用初始化完成且最近 1500 ms 内有有效 TWCC 的估计；启动阶段、反馈过期和 RR-only 连接保持原分配。
- 当前支持整条视频订阅暂停与恢复，尚未实现 simulcast/SVC 层选择；保留音频不代表预算不足时仍能满足全部音频需求。
- 接收侧质量评估及 RR 发送仍由 SR 触发，公共 Tick 尚未接入周期质量评估；本次提前的是评估器创建时机。
- 平滑质量指标用于观察趋势，质量降级、NACK/PLI 和拥塞控制仍读取原始反馈；源码率的平滑结果用于订阅需求估算。

## 2026-11-05 — 带宽算法扩展框架与发送反馈超时

### 本次完成

- 新增 `IPacketGroupObserver` / `PacketGroupObserver`，在匹配发送历史后的 TWCC 入口提供分组观测接口；可向现有码率控制传递拥塞状态、时延趋势、接收速率与丢包率，未提供的结果沿用原有逐包估算。
- 新增 `IProbeController` / `ProbeController`，提供探测决策、反馈及执行事件接口；支持接管探测、暂不探测或退回默认策略。
- `NetworkControllerConfig` 提供算法工厂，每条下行 Transport 独占算法实例；同一连接内的音视频共享估算，算法计算由发送控制器的锁串行保护。
- `DownlinkTrackManager` 执行探测计划，限制速率、时长、字节预算与单次突发；`RtpSenderTrack` 将探测编号写入本地发送历史，TWCC 匹配后可关联原探测簇。
- 接通探测开始、完成与中止事件；退订清理承载轨道，切网、断网及反馈超时清理算法状态，并过滤旧路径或旧探测代次的执行事件。
- 复用现有媒体 Worker 的周期任务检查发送反馈超时，默认 5000 ms；撤销旧估计并通知一次空更新，保留订阅、连接及连续序号。RR 持续而 TWCC 长期缺失时单独重置 TWCC 历史。
- 发送历史按年龄清理，保留近期在途包；补充反馈恢复、算法回退、探测执行与 Transport 隔离测试。

接口约定、算法填写入口及工厂示例见[分组观测与主动探测算法扩展框架](reference.md#分组观测与主动探测算法扩展框架)，
超时行为见[反馈超时与重新估算](reference.md#反馈超时与重新估算)。

### 验证情况

- `Packetia` 及相关测试目标构建通过；`test_weak_network_manual`、`test_rtp_input`、`test_udp_binding`、`media_core`、`test_webrtc_crypto`、`webrtc_service` 共 6 组 CTest 通过。
- 新增 7 项框架测试及 8 项反馈超时测试；`git diff --check` 通过。

复用已配置的集成构建目录执行回归：

```sh
cmake --build build/webrtc-integration --target Packetia test_weak_network_manual test_rtp_input test_udp_binding test_media_core test_webrtc_crypto test_webrtc_service -j4
ctest --test-dir build/webrtc-integration --output-on-failure -R '^(test_weak_network_manual|test_rtp_input|test_udp_binding|media_core|test_webrtc_crypto|webrtc_service)$'
```

### 待实现

- `PacketGroupObserver.cpp` 中的分组规则、去重、有界窗口与跨组趋势判断，以及 `ProbeController.cpp` 中的触发、退避与容量判断均保留 `TODO(algorithm)`，由后续算法实现补齐。
- 默认继续使用现有 OU 逐包估算与受限 padding 恢复探测；本次未接入实际 Pacer、simulcast/SVC 选层，也未验证新算法效果、真实浏览器弱网或容量。
