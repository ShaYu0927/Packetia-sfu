# Packetia SFU

基于 C++17 的音视频服务学习与实验项目，围绕房间、发布订阅和 RTP/RTCP 转发实现 SFU，
并提供录制、转码、混流及 AI 处理组件的接入边界。

项目正在逐步拆清媒体核心、业务控制和处理组件的职责。当前支持单进程运行，以及独立控制服务与多个 SFU 节点；
处理组件默认仍在单进程入口内运行，外部 Agent、Egress 和处理任务 RPC 尚未实现。

- [当前能力与限制](#当前能力与限制)
- [架构与模块](#架构与模块)
- [集群现状与演进方向](#集群现状与演进方向)
- [构建与依赖](#构建与依赖)
- [运行方式](#运行方式)
- [发送侧控制与恢复](#发送侧控制与恢复)
- [日志](#日志)
- [近期改动与验证](#近期改动与验证)
- [处理组件与构建目标](#处理组件与构建目标)
- [测试](#测试)
- [后续拆分方向](#后续拆分方向)
- [详细文档](#详细文档)

## 当前能力与限制

| 模块 | 已有能力 | 当前边界 |
| --- | --- | --- |
| WebRTC / SFU | WebSocket 信令、H264/Opus 协商、ICE-Lite、DTLS/SRTP、独立发布与订阅双会话，三端 Chrome 同时收发验证通过 | 每个订阅会话最多 8 条轨道；动态轨道重协商尚未开放 |
| RTP / RTCP | 接收排序、H264 组帧、NACK、PLI/FIR、TWCC/RR、发送侧带宽控制与 WebRTC 有界 Pacer | simulcast/SVC 选层和 FEC 尚未接入；其他视频格式的完整 RTP 接入需分别完善 |
| 断线恢复 | 信令恢复、参与者保留、同会话 ICE restart | 同进程内恢复，不包含服务重启或跨节点媒体迁移 |
| TURN / STUN | UDP 中继、长期凭证认证、权限与通道、IPv4/IPv6 | 不支持 TURN TCP/TLS；TLS 传输组件尚未接入 WSS/TURN 监听 |
| 录制 | H264/AAC fMP4 切片、逐流启停、SQLite 索引与查询 | 原始流录制与处理输出的保存需要分别组装 |
| 视频转码 | FFmpeg 软件编解码，支持 H264/H265/VP8/VP9/AV1，一次解码生成多档输出 | 依赖本机 FFmpeg 实现；尚无远程任务 API、派生流 RTP 输出与浏览器播放闭环 |
| 混流 | 输入配置、任务生命周期、有界队列及输出接口 | 实际拼画面、混音和编解码后端需要实现 `IMixBackend` |
| AI | 模型提供者接口、请求编排与帧处理入口 | 默认提供者未配置模型，不会执行实际推理 |
| 语音 Agent 框架 | Control 内的会话创建、查询、停止与房主权限检查 | 仅管理内存元数据，Agent 入房、gRPC、音频及 ASR/LLM/TTS 尚未接通 |
| 控制面 | 账号与会话、房间管理、节点注册与心跳、房间分配、JWT 准入 | 单控制服务与 SQLite；同一房间固定一个 SFU，尚无跨节点媒体分发和节点故障迁移 |
| 监控 | HTTP 指标快照、Worker、发送队列与进程资源指标 | AI、混流和转码自有队列的指标尚未统一覆盖 |

RTSP、RTMP、SIP 等协议代码与服务入口保留在项目中，具体接入和互通边界见[技术参考](docs/reference.md)。
下行发送轨道将 RTP 序号与恢复映射、重传缓存和反馈状态分开管理；WebRTC 的 TWCC 发送历史在 UDP socket 接受数据后记录，
切网或关闭导致失效的排队任务不会写入新路径历史。RTP 重传目前仍使用原包，RTX 协商与构包尚未接通。
编解码计算层支持某种格式，并不意味着该格式已经贯通协议、容器和浏览器播放。
项目尚未完成系统容量压测，配置中的会话上限不代表实际承载能力。

## 架构与模块

```text
应用入口与组装
├─ Packetia：网络服务 + 媒体核心 + 本地处理组件
├─ PacketiaControl：账号、会议控制、节点调度与入房凭证
└─ PacketiaSfu：房间运行时、连接管理与媒体转发

媒体数据
RTP 接收 / 解包 → 完整编码帧 → EncodedFrameRouter
                              ├─ 录制
                              ├─ AI 帧处理入口
                              ├─ 混流任务 → 独立输出路由
                              └─ 转码任务 → 独立输出路由
```

`ServerApp` 负责应用资源、配置协调和生命周期，通过服务接口控制处理组件。
`MediaProcessingServices` 描述组件集合与输出路由；默认工厂只创建对象，实际线程和任务由各服务管理。
服务启动失败会回滚，停机按依赖顺序排空资源。

房间、成员、媒体会话和发送队列的归属及释放顺序见[资源生命周期](media/endpoint/RESOURCE_LIFECYCLE.md)。
临时 WebSocket 断开保留成员及媒体等待恢复；退出或结束房间时，先关闭发送与协议，再解除轨道关系和成员身份。

| 目录 | 主要职责 |
| --- | --- |
| `server/` | 应用启动、信令接入、HTTP 接口和进程组装 |
| `service/core/` | 通用服务生命周期 |
| `service/meeting/`、`service/Cluster/` | 会议业务、节点租约和房间调度 |
| `service/processing/` | 处理组件接口集合、工厂类型和默认本地组装 |
| `service/RecordService/` | 录制、文件、切片及索引 |
| `service/AIService/` | 模型接口、AI 编排、帧入口和语音 Agent 会话框架 |
| `service/ConferenceMixService/` | 混流任务和后端接口 |
| `service/TranscodeService/` | 转码任务、队列和运行时 |
| `media/` | 媒体类型、流与轨道、帧路由、端点、质量控制和 codec 适配 |
| `Rtsp/`、`protocol/` | 协议、RTP/RTCP、ICE、WebRTC、TURN 等实现 |
| `network/`、`utils/` | 网络传输、EventLoop、线程与有界队列 |
| `config/`、`monitoring/` | 配置快照与运行指标 |

## 集群现状与演进方向

当前已实现“多个房间分配到多个 SFU 节点”，单个房间的在线成员与媒体运行时仍由一个节点管理：

```text
客户端 → PacketiaControl：检查权限、查询或分配房间归属、签发入房令牌
                       ├─ 房间 A → SFU 1
                       └─ 房间 B → SFU 2
客户端 → 目标 SFU：WebSocket 信令与 WebRTC 媒体连接
```

`POST /api/v1/room-admissions` 返回 `signaling_url` 和令牌；客户端随后连接对应 SFU，
媒体无需经过 Control。已有房间沿用原绑定；新房间从租约有效且房间容量未满的节点中选择，
主要比较已绑定房间数与 `max_rooms` 的比例，其次比较参与人数。

| 状态 | 当前归属与存储 |
| --- | --- |
| 房间业务权限、节点注册与租约、房间绑定 | Control 的 SQLite 数据库，由事务协调分配 |
| 在线成员、发布轨道、订阅关系 | 房间所属 SFU 的内存；管理请求先定位节点再转发 |
| 连接、TWCC、Pacer、发送历史 | 实际服务该连接的 SFU 本地状态 |

`node_id` 标识节点，`instance_id` 区分进程实例，`assignment_id` 区分房间分配代次。
集群节点分配与进程内 Owner 分配是两层职责：前者决定房间在哪个节点，后者决定在哪个执行线程。
当前使用房间哈希固定 Owner，Owner 内串行，多个 Owner 和媒体 Worker 并行；WebRTC 共用 UDP 收发入口。

后续重点是“单个逻辑房间跨多个 SFU”，先面向少量发布者、大量观众设计。以下为目标结构，尚未实现：

```text
                  同一个逻辑房间
主播 → 源 SFU ──┬─→ 接入 SFU 1 → 部分观众
                ├─→ 接入 SFU 2 → 部分观众
                └─→ 接入 SFU 3 → 部分观众
```

需要增加跨节点媒体转发、源流目录、按用户分配接入节点，以及节点间的权限和房间状态协调。
每个接入节点维护自己的用户连接与反馈状态；房间规则和流目录按需共享。
多人发布场景还需限制每个用户实际订阅的流数。多个大房间是在这些分发关系上继续做负载分配。

Redis、MySQL/PostgreSQL 或 OceanBase 都属于后续存储方案评估，目前均未接入；
跨机器部署多个 Control 需要明确共享状态的权威存储、原子分配与故障处理规则，不能直接共享本地 SQLite 文件。
增加数据库节点或同机 Docker 容器不等于增加媒体出口容量。

下一步先验证现有集群入房路径并测量节点容量，再以“两台 SFU 服务同一房间的一路源流”为跨节点分发的首个验收目标。
当前尚无万人房间、多房间大规模并发或公网弱网容量的实测结论。
控制服务与 SFU 的状态归属、租约过期行为及真实浏览器故障测试见
[控制服务与媒体服务的职责边界](docs/control-media-boundaries.md)。
部署入口见[集群 README](deploy/cluster/README.md)，源码与接口见[会议控制与节点调度](docs/reference.md#会议控制与节点调度)。

## 构建与依赖

需要支持 C++17 的编译器、CMake 3.16 以上版本、`pkg-config`、OpenSSL、SQLite 3.24 以上版本，
以及 FFmpeg 的 libavformat、libavcodec、libavutil、libswscale 开发库。
默认启用 libwebsockets；WebRTC 媒体运行需要 libsrtp2，控制面 HTTP 客户端需要 libcurl。

`BUILD_TESTING` 默认开启，需要 GoogleTest。录像集成测试另需 `ffmpeg` 命令行、Python 3 及相应编码器。
项目默认使用系统 FFmpeg 开发包，也支持从 `third/ffmpeg` 编译后通过 `PACKETIA_FFMPEG_ROOT` 接入。
源码构建、组件选择和录制 / 转码复测见[FFmpeg 源码构建说明](third/ffmpeg/README.md)。
以下命令均从仓库根目录执行。

### Linux

Ubuntu / Debian：

```sh
sudo apt update
sudo apt install build-essential cmake pkg-config libssl-dev libsqlite3-dev \
  libavformat-dev libavcodec-dev libavutil-dev libswscale-dev \
  libwebsockets-dev libsrtp2-dev libcurl4-openssl-dev libgtest-dev

cmake -S . -B build/dev -DBUILD_TESTING=ON
cmake --build build/dev --parallel
```

### macOS

安装 Xcode Command Line Tools 和 Homebrew 后：

```sh
brew install cmake pkgconf googletest openssl@3 ffmpeg sqlite libwebsockets libsrtp

cmake -S . -B build/dev -DBUILD_TESTING=ON \
  -DCMAKE_PREFIX_PATH="$(brew --prefix)" \
  -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)" \
  -DPACKETIA_LIBWEBSOCKETS_ROOT="$(brew --prefix libwebsockets)"
cmake --build build/dev --parallel
```

### 常用构建选项

| 选项 | 默认值 | 用途 |
| --- | --- | --- |
| `BUILD_TESTING` | `ON` | 构建测试并查找 GoogleTest |
| `PACKETIA_WITH_LIBWEBSOCKETS` | `ON` | WebSocket 信令和相应 HTTP 后端 |
| `PACKETIA_WITH_SRTP` | `ON` | 查找并启用 libsrtp2 媒体后端 |
| `PACKETIA_BUILD_RECORDING_TESTS` | `OFF` | 额外的 FFmpeg 录像集成测试 |
| `PACKETIA_FFMPEG_ROOT` | 空 | 指定自编译 FFmpeg 安装目录；头文件、容器 / 编解码库与缩放库统一从该目录查找 |

编译仓库内的 FFmpeg 并用于主程序：

```sh
python3 scripts/build_ffmpeg.py --jobs 4
cmake -S . -B build/ffmpeg-app -DPACKETIA_FFMPEG_ROOT="$PWD/build/ffmpeg/install"
cmake --build build/ffmpeg-app --target Packetia --parallel 4
```

基础构建包含常用解码、AAC 编码、容器与缩放库；H264/H265 等软件编码需显式启用外部 codec。
脚本使用独立的源码、工作和安装目录，不写系统目录，构建产物留在已忽略的 `build/` 中。

只构建需要的程序：

```sh
cmake --build build/dev --target Packetia --parallel
cmake --build build/dev --target PacketiaControl PacketiaSfu --parallel
```

`PacketiaControl` 需要 libwebsockets，`PacketiaSfu` 还要求检测到 libsrtp2。
关闭相关依赖后，不会生成所有 WebRTC 示例、集群入口和测试目标。

## 运行方式

### 单进程开发

```sh
./build/dev/Packetia --check-config
./build/dev/Packetia
```

首次配置时，CMake 将 [config/packetia.json](config/packetia.json) 复制到构建目录；后续构建保留其中的修改。
默认开发配置启用账号服务和本机 HTTP API，监听 `127.0.0.1:8081`，RTSP 端口为 `8554`。
Ctrl+C 会停止服务并排空资源。

```sh
./build/dev/Packetia --config config/packetia.http-local.json
./build/dev/Packetia --help

# 未登录时预期返回 401。
curl -i http://127.0.0.1:8081/api/v1/me
```

配置查找顺序为 `--config`、`PACKETIA_CONFIG`、可执行文件旁的 `packetia.json`、当前目录的 `packetia.json`、
`config/packetia.json`，最后使用内置默认值。环境变量可覆盖对应配置。
数据库、录像等相对路径按进程工作目录解析，建议从仓库根目录运行。

完整字段见 [AppConfig](config/AppConfig.h) 和[配置示例](config/packetia.example.json)。
内置默认值与仓库开发配置不同；部署时应明确设置监听地址、凭据和功能开关。

### 控制服务与多个 SFU 节点

```sh
cmake --build build/dev --target PacketiaControl PacketiaSfu --parallel
python3 scripts/run_cluster_local.py --build-dir build/dev --nodes 2
```

启动脚本生成本地凭据、配置和节点日志，并运行一个控制服务与两个 SFU。
也可以分别执行 `build/dev/server/PacketiaControl CONFIG.json` 和 `build/dev/server/PacketiaSfu CONFIG.json`。
配置模板与容器定义位于 [deploy/cluster](deploy/cluster)。

Docker 部署支持生成节点配置与持久密钥、健康检查启动顺序，以及 Caddy HTTPS/WSS 入口：

```sh
python3 deploy/cluster/deploy.py init --domain rtc.example.com --public-ip 203.0.113.10
python3 deploy/cluster/deploy.py up --build
python3 deploy/cluster/deploy.py check
```

替换示例域名和地址，详细端口、更新方式和客户端接入见 [Docker 部署](deploy/cluster/README.md)。

只有公网 IP 时，可使用 Caddy 测试 CA 提供 HTTPS/WSS，并复用已加载的后端镜像：

```sh
python3 deploy/cluster/deploy.py init --domain '' --public-ip 203.0.113.10 --tls-mode internal
python3 deploy/cluster/deploy.py up --single-sfu --recreate
```

客户端需导入 Caddy 根证书，导出和信任步骤见上述部署说明。
反向代理提供后端 API 和信令入口，不包含测试网页；媒体直接通过 SFU 的 UDP 端口传输。

单进程入口提供账号 API 和本地服务；会议创建、成员管理、节点分配和入房 JWT 签发由 `PacketiaControl` 提供。
独立 `PacketiaSfu` 当前专注房间和媒体转发，本地录制、AI、混流、转码组件尚未接入该入口。
接口和权限说明见[会议控制与节点调度](docs/reference.md#会议控制与节点调度)。
登录、建房、入会许可的额外业务限制集中在 `server/api/BusinessAccessPolicy.cpp`，
原有账号凭证、房主和房间成员权限检查继续由各自服务执行。

浏览器示例位于 [examples/webrtc-room](examples/webrtc-room)，启用 WebSocket 和 libSRTP 时可构建
`PacketiaRoomDemo`。选择 Participant 后可同时发布摄像头、麦克风并接收其他参与者，
发布和订阅可分别停止、重建；推流参数、实时质量曲线、JSON 导出及算法测试入口见
[传输实验室 README](examples/webrtc-room/README.md)。房间运行步骤见[浏览器会议与 TURN 示例](docs/reference.md#浏览器会议与-turn-示例)。

## 发送侧控制与恢复

一个下行连接共享发送侧带宽预算，分配器再决定该连接上各条订阅是否允许转发：

```text
接收端 RTCP/TWCC 反馈 + 本地发送历史
    → PacketGroupObserver：包分组、时延差 → 引入的 WebRTC TrendlineEstimator
    → TransportBweController / RemoteBitrateController：链路目标码率
    → SendSideController：网络状态与发送预算
    ├─ DownlinkBandwidthAllocator：准备 → 试分配 → 提交
    │    → DownlinkTrackManager：暂停 / 等待关键帧 / 转发
    │    → ConsumerTransport：检查订阅权限及排队包有效性
    └─ PacedSendQueue：按发送速率释放 RTP → UDP 发送队列 → socket
```

分配器借鉴 LiveKit 的阶段划分，当前仍对单层视频做整流准入，音频优先预留，
视频按优先级分配。提交时先扣除尚在暂停等待期内的发送占用，再恢复其他轨道。
默认使用目标带宽的 85%，暂停等待 300 ms，恢复等待 1000 ms，并要求 20% 恢复余量。
过期或无效的预算保留已有视频允许状态；恢复余量不会计入实际请求带宽。
源码对照、带宽变化量和具体算例见[下行分配阅读指南](docs/downlink-allocation-livekit.md)。

趋势检测已引入固定版本的 WebRTC `TrendlineEstimator`，保留原始源码、许可证与适配版本，
并通过测试对照默认配置的逐步输出。包分组、接收速率估计和码率控制仍使用本项目实现。
源码位置、裁剪差异和断点阅读顺序见[WebRTC 趋势检测学习指南](docs/webrtc-trendline.md)。

房间订阅已接通下行 TWCC 协商：独立声明头扩展和 `transport-cc` 反馈能力，与订阅浏览器的 Offer 取交集，
再将 Answer 中的扩展 ID 配给 `RtpSenderTrack`。只有两项能力同时协商成功才启用编号，
同一下行连接共享序号分配器；未协商成功时保留 RR 回退。该实现不要求发布端支持 TWCC。

上行发布也声明音频、视频的 TWCC 扩展与 `transport-cc` 反馈能力，Answer 保留 Offer 的扩展 ID 并使用 `recvonly` 方向；
仅在双方协商出扩展和反馈时保留 `transport-cc`，对未提供 TWCC 的客户端保持兼容。
当前仅完成上行 SDP 协商，接收 Transport 的到达记录、反馈定时生成与 RTCP 回传尚未接通，不能据此认为上行拥塞控制已经工作。

暂停视频的恢复探测已接通：计划 → RTP Padding → WebRTC Pacer 临时提速 → 发送完成历史 → TWCC 簇评估 → 更新共享预算。
成功后仍经过分配器等待和关键帧恢复；丢包或拥塞撤销提速，反馈不足则超时退避。
预算参数、测量口径、源码入口和测试见[下行带宽探测闭环](docs/bandwidth-probing.md)。

`PacedSendQueue::GetStats()` 提供以下本地诊断值：

| 字段 | 口径 |
| --- | --- |
| `queued_bytes` / `queued_packets` | 容量占用，包含已出队但尚未返回的消费回调 |
| `oldest_wait_ms` | 当前队列中最老包的等待时长，包含控制包 |
| `rejected_packets` | 因无效输入、队列容量或内存分配失败而拒绝的累计包数 |
| `queued_rtp_bytes` | 仍在音频、媒体队列中等待的字节数，不含控制包及已出队包 |
| `pacing_bitrate_bps` | 当前配置的 pacing 速率，单位 bit/s |
| `estimated_drain_ms` | `ceil(queued_rtp_bytes × 8000 / pacing_bitrate_bps)`；空队列为 0，有积压且速率为 0 时无值 |

预计排空时间假设没有新包入队、速率不变，不包含 pacing 债务和 socket 队列，不能作为端到端时延。
Pacer 之后的 `UdpSendQueue` 同样有容量限制，默认最多 4096 包、4 MiB，入队满 100 ms 的包在出队时丢弃。
有效性检查、发送和完成回调异常会被隔离；Socket 背压时丢弃当前包并继续，不阻塞网络线程或无限重试。
`GetStats()` 提供本地队列统计，HTTP 指标中的 `send.udp.dropped_packets_total` 提供按原因分类的丢弃计数，
具体口径见[网络发送与队列说明](docs/reference.md#网络层边界与线程约定)。
`PacedSendQueue` 的统计已接入可选的服务端 QoS 诊断日志，记录反馈、控制决策、分配和队列状态；
开启方式、字段口径与复测步骤见[媒体质量诊断日志](docs/media-quality-diagnostics.md)。
浏览器面板仍使用自身 `getStats()`；逐连接的 BWE、Pacer 与探测诊断尚未通过 HTTP 接口提供。

带宽恢复后，视频进入 `WaitingKeyFrame`，请求上游关键帧并等待可恢复画面。
当前 `Tick()` 将下一次 PLI 重试设为一秒后；实际发出还受 Tick 调用时机和请求限流影响。
[关键帧重试策略](media/quality/KeyFrameRetryPolicy.h) 提供独立计算函数
`ComputeKeyFrameRetryIntervalMs(publisher_rtt_ms)`：有效发布端 RTT 按两倍计算，
限制在 200–1000 ms 并向上取整；未知、非有限值或非正数回退为 1000 ms。
例如 RTT 为 150 ms 时返回 300 ms，30 ms 时返回 200 ms。
`SfuEndpoint::OnTrackPli()` 已使用发布端反馈快照中的 RTT 计算限流间隔，
并通过同一发布轨道共享的 `KeyFrameRequestLimiter` 合并多个订阅者的请求。
`DownlinkTrackManager::Tick()` 的重试调度仍固定为一秒，限流间隔变短不会自动提高它的触发频率。
Simulcast/SVC 选层、RTT 驱动的完整重试调度和多协议适配仍待后续实现。

## 日志

日志线程从有界队列批量写出，支持文本/单行 JSON、级别过滤及文件轮转。
集群入口记录服务名和 SFU 节点 ID；QoS 事件提供房间、成员、会话与订阅关联，通用请求上下文尚未统一接入。

```sh
PACKETIA_LOG_MODE=console PACKETIA_LOG_FORMAT=json PACKETIA_LOG_LEVEL=info \
  python3 scripts/run_cluster_local.py --build-dir build/dev --nodes 2
```

原生程序默认使用 `both`（控制台和文件）、`text`、`info`；本地集群启动器固定使用控制台输出并保存各进程日志。
Docker Compose 默认使用控制台 JSON 日志，交给 Docker 保存和轮转。
`PACKETIA_LOG_MODE` 决定写入控制台（`console`）、应用文件（`file`）还是两者（`both`）；
应用文件默认为进程工作目录下的 `app.log`，可用 `PACKETIA_LOG_FILE` 指定路径。
Docker 的 `json-file` 驱动保存控制台输出，与应用自身文件输出是两种机制。
配置默认值见 [LoggerConfig.h](log/LoggerConfig.h)，结构化字段、队列和轮转实现见 [logger.h](log/logger.h)。

HTTP 适配器复用该日志器，记录启动/停止、关键运行失败和请求终态。
请求字段包括方法、路径、状态码、耗时和正文字节数；异步响应、重复回调和长连接不会重复记录同一次请求。
普通请求使用 INFO，5xx 和传输故障使用 ERROR，成功的 `/healthz`、`/readyz` 使用 DEBUG。
查询参数、正文、Authorization 和 Cookie 不写入日志；尚未写出完整响应头的中止请求状态码为 `null`。
详细事件、查看和导出命令见 [Docker 日志说明](deploy/cluster/README.md#http-核心日志)。

通过 `PACKETIA_QOS_LOG=1` 开启媒体诊断，配合 JSON 与 INFO 输出，记录下行连接约每秒一次的反馈、
目标码率、实际发送完成速率与队列快照，以及轨道和探测状态事件。未知指标输出 `null`。
浏览器示例默认开启，原生与集群入口默认关闭；示例目录中运行 `npm run qos:summary` 可汇总日志。
字段口径和与浏览器报告的关联方法见[媒体质量诊断日志](docs/media-quality-diagnostics.md)。

## 近期改动与验证

### 2026-11-26：弱网观测与 TURN 实验整理

- `OUTrendDetect` 的抖动补偿改成随时间指数衰减的峰值；乱序样本不回退参考时钟，
  长时间采样间隔重建观测基线，连续过载需同时满足时间与不同到达时刻的样本数要求。
  新增乱序、时间戳回绕、间断采样、重置和过载确认测试；这些检查不代表公网弱网效果已经验证。
- TURN 增加双分配之间的中继测试，并保留 relay-only 浏览器验证脚本、可选 Linux Compose 和凭证模板。
  浏览器脚本覆盖双向音视频、数据通道和 ICE restart；需另行提供 TURN 服务或已有测试程序。
- 移除独立 TURN 程序入口与无有效模块映射的 libhv Git 引用；TURN 协议库仍保留。
  独立 TURN 程序、Docker 中的对应程序，以及 GCC 论文实验目录均改为源码存在时才构建。
- `scripts/run_gcc2016_experiment.py` 保留外部实验程序的轨迹汇总工具；本仓库当前不包含对应复现源码，需通过 `--binary` 指定已有程序。

本轮未接入 gRPC 依赖、未启动独立 Agent，相关语音业务仍停留在会话框架阶段。

本轮验证：本机 Debug 构建通过 `PacketiaControl`、`PacketiaSfu` 及相关测试目标；
`TurnUnitTest`、`VoiceAgentUnitTest`、`AccountApiFrameworkUnitTest`、`test_weak_network_manual` 四组 CTest 全部通过。
部署脚本测试通过 16 项、跳过 1 项；TURN 浏览器脚本语法、Python 脚本编译及 Compose 配置检查通过。
尚未执行真实浏览器 TURN 中继、Docker 镜像构建或公网弱网效果验证。

### 2026-11-25：业务接入、WebRTC 初始化整理与语音 Agent 框架

- `WebRtcService::Start()` 保留状态检查与失败回收，组件组装集中到 `InitializeComponents()`，
  再按网络、鉴权、房间 owner、管理通道和信令入口拆成独立函数。房间仍在各自执行器上启动，信令巡检保持 100 ms。
- `RoomController` 将路由注册与处理函数分开；`BusinessAccessPolicy` 提供登录、创建房间、入会许可三个扩展点。
  当前包含 `banned_user` 和 `restricted_user` 的练习规则，策略允许后仍须通过原有鉴权，不会直接生成登录或入会凭证。
- 发布端音频和视频补充 TWCC 扩展与 `transport-cc` 的 SDP 能力协商，覆盖缺失反馈、缺失扩展、
  不支持的扩展 ID 和错误方向；上行到达记录与反馈发送尚未实现。
- `server/api/AgentController` 统一处理 HTTP 入口；`service::ai::VoiceAgentService` 位于 `service/AIService`，
  由 `PacketiaControl` 的 `ServiceManager` 管理，先于会议模块停止。头文件前向声明 `MeetingModule`，完整定义在实现文件中包含。

Agent 接口沿用账号登录的 Bearer token，全部使用 POST 与 JSON 请求体；客户端不能指定任务所有者或权限：

| 路径 | 请求体 | 成功结果 |
| --- | --- | --- |
| `/api/v1/agent-sessions` | `{"room_id":"..."}` | 201，返回 `session_id`、`room_id`、`state: prepared` |
| `/api/v1/agent-sessions/get` | `{"session_id":"..."}` | 200，返回会话快照 |
| `/api/v1/agent-sessions/stop` | `{"session_id":"..."}` | 200，返回 `state: stopped` 并删除记录 |

仅开放房间的房主可创建，只有创建者可查询或停止。每房间最多一个会话，默认总上限 128；
重复创建或超额返回 409，未知及其他账号的会话返回 404，登录失效返回 401，服务停止后返回 503。
停止后再次查询或停止返回 404。会话只保存在内存，服务关闭时清空；房间结束暂不会自动清理这些元数据。
接口目前只注册到 `PacketiaControl`，单进程 `Packetia` 未注册。

`prepared` 只表示元数据准备完成。下一步计划让独立 Agent 进程以机器人身份入房：
由 Control 检查房主权限，为机器人登记成员角色并签发房间 JWT，再由 Agent 连接目标 SFU 的 WSS，
使用 `packetia` 子协议发送 `join`，收到 `joined` 后才算实际入房。
机器人身份与发起者账号、SFU 返回的 `participant_id` 分开保存；后续 WebRTC 订阅用户音轨、发布回复音轨。
gRPC 可用于独立 Agent 的任务调度，实时媒体通过 WebRTC；这些入房与跨进程链路当前均未实现。
LiveKit Python SDK 不能直接使用 Packetia 的自定义信令协议。

填写业务逻辑从 `VoiceAgentService::CreateSession()` 开始；后续资源释放统一接入 `ReleaseSession()`。
这两处目前在会话锁内执行，网络调用、等待入房、解码和模型推理必须移到独立执行器并增加取消机制。

本机 macOS 上从本次提交源码编译 `PacketiaControl`、`PacketiaSfu` 成功；
`AccountApiFrameworkUnitTest`、`MeetingModuleUnitTest`、`ClusterApiUnitTest`、`VoiceAgentUnitTest`、
`webrtc_service`、`webrtc_twcc`、`webrtc_control`、`webrtc_members`、`webrtc_audio_controls`、`webrtc_duplex`
共 10 个 CTest 入口通过，其中新增 Agent 测试包含 8 个用例。本轮未验证真实 Agent 入房、公网媒体或容量。

### 2026-11-24：房间与媒体资源归属、释放顺序

- 完整退出时先关闭 WebRTC 发送适配器和 Pacer，再在房间 owner 上同步停止 ICE、DTLS 与 SRTP，随后撤销轨道关系、端点及发布流注册，最后移除成员。
- 下行协商参数改为弱引用传输对象，活动订阅持有发送所需引用；闲置配置不再延长连接寿命，过期配置在清理时释放 SSRC 占位。
- 临时 WebSocket 断开仍保留成员与媒体等待恢复；单条端点订阅取消后可复用有效协商参数，其他轨道共用的连接继续运行。
- 新增传输引用释放与重新订阅隔离测试；结束房间测试覆盖活动发布端和断线订阅端，并保留外部端点引用检查显式停止与注册清理。

本机 macOS 上 `test_rtp_input`、`webrtc_service`、`webrtc_members`、`webrtc_control`、`webrtc_duplex`、
`webrtc_audio_controls`、`webrtc_twcc` 共 7 个 CTest 入口通过；本轮未执行公网或容量测试。
账号注销与已有 SFU 成员清理尚未联动。对象归属、操作差异、关闭顺序及复测命令见[资源生命周期](media/endpoint/RESOURCE_LIFECYCLE.md)。

### 2026-11-23：公网反向代理、HTTP 日志与接口验证

- Docker 部署支持公网 IPv4 + Caddy 内部测试 CA，以及自定义 HTTPS/WSS 端口；`--single-sfu` 可只启动 Control、SFU A 和代理，已有镜像直接复用。
- Caddy 为 IP 客户端设置 `default_sni`，解决无 SNI 且 Docker 内部地址与公网 IP 不同时的证书选择失败；80 端口跳转与准入返回的 WSS 地址均包含配置的公网 HTTPS 端口。
- 补齐镜像的 `libswscale` 构建/运行依赖；HTTP 后端检测 libwebsockets 自定义请求头枚举 API，兼容 Debian bookworm 的 4.1 版本。
- HTTP 适配器新增结构化生命周期、请求完成和中止日志，复用现有异步日志器；记录超时、提前断开、无效响应和响应预算不足等原因。
- 新增 `scripts/check_public_api.py`：以独立账号/房间检查公开 API 和 WSS，报告脱敏；结束测试房间并撤销会话，随机账号记录保留。
- 镜像导出包、构建目录、运行配置、密钥和测试产物不进入源码提交；镜像包同时从 Docker 构建上下文排除。

验证记录：HTTP 集成测试 13 项及账号、路由测试通过；部署配置和无 SNI 的 IP TLS 回归通过。
本轮公网接口测试覆盖 15 个公开 HTTP 方法/路径，97 次 HTTP 请求、13 次 WSS 请求和 12 项响应语义检查，合计 122 项通过。
公网接口检查跳过了 TLS 证书校验；不包含真实音视频、弱网、并发容量或跨节点故障验证。
当前 `/api/v1/metrics` 允许匿名读取，内部节点管理路由未暴露到 Caddy 公网入口。
部署文件职责、端口映射、配置修改生效方式和日志导出见 [Docker 部署说明](deploy/cluster/README.md)。

### 2026-11-23：移除缺失的独立 RR 控制器依赖

发送侧恢复使用 `NetworkController` 内置的 RR 回退策略，移除缺失源文件的构建和头文件引用。
每次反馈按网络质量调整目标：Excellent 增加 5%，Good 保持，Weak 降低 15%，Bad 降低 35%，并限制在配置范围内。
该基础策略依赖反馈频率，不包含按时间增加和 RTT 冷却窗口。
TWCC 目标优先并同步回退基准，结果继续输出给现有 Pacer；RR 不被当成有效链路容量用于订阅分配。

### 2026-11-20：NACK 缺包调度与下行批量重传

接收侧 NACK 改为按请求到期时间选择批次，避免连续丢包超过单批上限时，首批包的重复请求长期挤占其他缺包的首次请求。
首次请求与重试使用同一优先级口径，保留乱序等待、RTT 间隔、恢复期限和批次上限。
新增连续丢包、序号回绕及新缺包与旧重试竞争的回归测试；链路入口、默认参数和未完成场景见 [NACK 业务场景与代码阅读](docs/nack-recovery.md)。

下行反馈改为有界请求列表和合并通知，复用订阅发送轨道的媒体 Worker；默认最多积压 512 个序号，每个任务最多处理 64 个。
投递失败或任务被队列淘汰后可由反馈或 Tick 重新调度；退订、关闭及转发不连续时旧任务不能继续重传。
新增队列与实际 Worker 接线测试，保留缓存包龄、重复重传限制、暂停闸门和 TWCC 行为。
该改动控制积压与处理批次，每个订阅者仍保存完整 RTP 重传缓存；共享源包缓存与下行改写映射尚未实现，内存与吞吐收益尚未压测。
补齐缺包表溢出与序号大跳的恢复通知：整段缺口无法登记时放弃待修复包，更新序号基准，复用已有回调请求上行关键帧。
恢复通知共用频率限制，PLI 继续使用发布轨道共享额度；新增容量、回绕、限频与端点接线测试，以及溢出和未登记序号统计。
本机 macOS 上 `test_weak_network_manual` 与 `test_rtp_input` 两个 CTest 入口通过，合计 303 个内部用例；本轮未执行公网弱网测试。

### 2026-11-19：FFmpeg 源码构建与统一依赖接入

- 新增 `scripts/build_ffmpeg.py`，从仓库内 FFmpeg 9.0.1 源码构建动态库，支持基础组件、可选软件 codec 及 `ffmpeg` / `ffprobe` 工具。
- 新增 `PACKETIA_FFMPEG_ROOT`，统一选择编解码、容器与缩放库及其头文件；指定安装缺少所需包信息或库时拒绝混用系统安装，未指定时仍使用系统开发包。
- 补齐被忽略规则遗漏的上游 Makefile 和安装所需文件，保留上游算法；工作目录、安装目录和日志均位于已忽略的 `build/` 下。
- 录制测试在指定安装时只使用该安装的命令行工具，构建参数、增量编译记录及复测步骤见 [FFmpeg README](third/ffmpeg/README.md)。

本机 macOS ARM64 上，基础版及启用 x264、x265、vpx、dav1d 和命令行工具的版本均构建成功，
Packetia 主程序构建通过，并确认动态库来自指定安装。配置测试 5 项、录制 CTest 8 项通过；
转码 CTest 3 项通过，其中内部用例 30 项通过、8 项跳过，实际验证了 H264、HEVC、VP8、VP9 编码。
本次未启用 AV1 编码器，Linux 构建尚未实测；主程序构建环境未发现 libsrtp2，不能据此认定 WebRTC 媒体运行链路已验证。

### 2026-11-19：UDP 发送健壮性与控制 / 媒体故障隔离

| 改动 | 实际行为 |
| --- | --- |
| UDP 发送队列 | 默认 100 ms 队列期限；检查队列与 Socket 使用同一执行器；背压时丢包并继续处理后续包 |
| 队列生命周期与异常 | 取消、析构和排队任务失效均结算已接受包；有效性与完成回调异常不阻断后续发送，成功发送不会因完成回调异常被改记为失败 |
| UDP Socket | 创建时设置非阻塞和 `FD_CLOEXEC`，有效重复创建时释放旧 FD；检查地址有效性与 `SO_REUSEADDR` 设置结果 |
| 丢弃诊断 | 新增队列快照及 `/metrics`、`/api/v1/metrics` 中的 UDP 丢弃原因分类，区分入队拒绝、发送背压和最终丢弃 |
| 关键帧请求限流 | `OnTrackPli()` 接入发布端 RTT 间隔计算，限流器支持每次传入间隔；策略头文件改为 include guard，`Tick()` 仍为一秒重试 |
| 集群故障验证 | Control 停机并等待租约过期后，拒绝新入房 / 恢复和内部管理请求；已有成员信令和已建立媒体仍可运行，续租后重新开放准入 |
| 浏览器集群检查 | 使用实际账号与入房 JWT 验证停机前、租约过期后和 Control 重启后的音视频；前端支持未配置 TURN 的直接连接 |
| 架构阅读说明 | 补充控制服务与 SFU 的调用顺序、状态归属、租约语义、探针用途及后续业务回调边界 |

本机 macOS 验证记录：

| 范围 | 已完成验证 |
| --- | --- |
| 构建 | `Packetia`、`PacketiaControl`、`PacketiaSfu` 构建通过 |
| 网络、传输与运行指标 | 10 个相关 CTest 入口通过，覆盖 UDP 生命周期、队列超时 / 背压 / 并发取消、回调异常、WebRTC 及指标接口 |
| 集群 | `ClusterUnitTest`、`ClusterApiUnitTest`、`MeetingControlIntegrationTest` 共 3 个 CTest 入口通过 |
| 浏览器统计与日志汇总 | JavaScript 单元测试 11/11 通过 |
| 真实浏览器故障检查 | 合成摄像头 / 麦克风通过真实 ICE、DTLS/SRTP 和 SFU 转发；Control 停机及租约过期后，发布字节、音频接收字节与视频解码帧数增长，参与者和媒体会话 ID 保持不变 |

复测命令与操作边界见[控制服务与媒体服务的职责边界](docs/control-media-boundaries.md)。
可选浏览器测试使用 `scripts/test_meeting_control.py --media-node node`，
通过 `--media-report examples/webrtc-room/artifacts/cluster-outage-result.json` 保存计数报告；
测试自动启动临时 Control/SFU 和页面，无需提前启动演示开发服务器。
报告目录已被 Git 忽略，不保存登录凭证、JWT、恢复令牌或 SDP。

控制租约与媒体转发原本已有分离，本次补齐了回归测试与真实媒体验证；
未新增跨节点媒体迁移、Control 高可用或完整 Full ICE，也未完成 Linux、公网弱网、长稳和万人并发容量验证。

### 2026-11-18：媒体诊断与 TWCC 接入

| 改动 | 实际行为 |
| --- | --- |
| 服务端 QoS 诊断 | 串联网络反馈、控制决策、轨道分配、发送完成量、队列与探测事件 |
| RR-only 发送计数 | 无 TWCC 时也统计实际发送完成量，不虚构传输序号与 TWCC 历史 |
| 独立下行 TWCC 协商 | 发布端未协商 TWCC，订阅端仍可协商并启用下行反馈 |
| 浏览器与服务端日志关联 | 按房间及进程/节点/连接标识，生成本次测试的原始日志与汇总 |
| 严格浏览器检查 | `PACKETIA_EXPECT_TWCC=1` 要求反馈匹配历史、产生斜率和有效分配预算 |

本机 macOS 验证通过：传输/算法/日志 CTest 5/5 组、信令 CTest 6/6 组、JavaScript 测试 11/11，
以及严格 TWCC 浏览器质量检查。首次订阅的最后周期样本为 320 份 TWCC、1653 个匹配包，
重新订阅为 96 份 TWCC、662 个匹配包；未匹配回执项均为 0，两次订阅均有有效分配预算。
浏览器测试包含模拟摄像头/麦克风、真实 SFU 收发、编码上限调整、ICE restart 和 720p60 请求。
这些结果验证本机收发与算法输入链路，不代表已经验证弱网恢复探测或大规模集群容量。

测试产物位于已忽略的 `examples/webrtc-room/artifacts/`，包括 `quality-result.json`、
`quality-server-qos.jsonl` 和 `quality-server-summary.json`；每次运行会更新，不作为固定基准。
复测步骤见[传输实验室 README](examples/webrtc-room/README.md)。

## 处理组件与构建目标

组件各自在目录内声明源文件和依赖，应用入口由 `server/CMakeLists.txt` 组装。

| 目标 | 职责 |
| --- | --- |
| `packetia_processing_api` | 处理服务接口和公共类型，不链接处理后端 |
| `packetia_recording_service` | 录制、封装和文件读取 |
| `packetia_ai_service` | AI 编排和帧入口 |
| `packetia_conference_mix_service` | 混流服务和任务运行 |
| `packetia_transcode_service` | 转码服务和任务运行 |
| `packetia_local_processing` | 默认本地组件组装 |
| `packetia_server_app` | 应用资源、网络服务及生命周期协调 |
| `Packetia` | 选择应用运行库与默认本地组装 |

主程序和应用集成测试共用 `packetia_server_app`，避免重复编译应用代码。
FFmpeg 由使用它的目标声明，接口、AI 和混流目标不链接 FFmpeg。
根项目配置阶段仍检查系统依赖；按组件构建可以只编译该组件及其依赖。

```sh
cmake --build build/dev --target packetia_ai_service --parallel
cmake --build build/dev --target packetia_recording_service --parallel
cmake --build build/dev --target packetia_conference_mix_service --parallel
cmake --build build/dev --target packetia_transcode_service --parallel
```

`packetia_processing_api` 是接口目标，供 `target_link_libraries` 使用。
旧 `service` 名称保留为接口聚合目标，兼容 `target_link_libraries(... service)`，不再生成单一的 `libservice.a`。
`Packetia` 和 `test_server_app` 仍输出到构建目录根部。

### 替换组件实现

[MediaProcessingServices](service/processing/MediaProcessingServices.h) 持有录制、AI、混流、转码接口及两个输出路由。
应用可以通过 `ServerApp` 的第三个构造参数选择工厂：

```cpp
server::ServerApp app(config, {}, service::CreateLocalMediaProcessingServices);
```

自定义工厂可复用 [CreateLocalMediaProcessingServices](service/processing/MediaProcessingServices.cpp) 的结果，再替换某个组件。
工厂接收共享输入路由、配置存储和录像事件接收器，在应用构造时调用一次，返回完整且处于 `Created` 状态的服务。
两个输出路由必须彼此独立，且与输入路由分开；启动、动态开关、失败清理和重启由应用管理。

这些接口用于进程内控制。`EncodedFrameRouter` 和 `unique_ptr<IMixBackend>` 等本地对象不能直接跨进程传输；
后续远程任务 API 需要独立的数据协议和媒体连接。

## 测试

完整默认测试需要先完成测试目标构建：

```sh
cmake --build build/dev --parallel
PACKETIA_LOG_MODE=console ctest --test-dir build/dev --output-on-failure
```

本次模块拆分的相关回归入口：

```sh
ctest --test-dir build/dev --output-on-failure \
  -R '^(server_app|server_launcher|server_config|startup_config|stream_services|ConferenceMixFramework|transcode_domain|video_transcoder|multicodec|webrtc_service|MeetingControlIntegrationTest)$'
```

发送侧质量控制、Pacer、日志与集群部署的回归入口：

```sh
cmake --build build/dev --target test_media_pacer test_webrtc_trendline test_weak_network_manual \
  test_rtp_input test_structured_logger test_webrtc_service PacketiaControl PacketiaSfu --parallel
PACKETIA_LOG_MODE=console ctest --test-dir build/dev --output-on-failure \
  -R '^(media_pacer|webrtc_trendline|test_weak_network_manual|test_rtp_input|StructuredLoggerTest|webrtc_twcc|webrtc_service|ClusterDeploymentConfigTest|MeetingControlIntegrationTest)$'
```

录像集成测试默认关闭，开启时需要 `ffmpeg` 命令行包含 libx264 和 AAC 编码能力：

```sh
cmake -S . -B build/dev -DPACKETIA_BUILD_RECORDING_TESTS=ON
cmake --build build/dev --parallel
ctest --test-dir build/dev --output-on-failure -R 'recording|mp4_reader'
```

测试范围、协议实验和历次验证记录保留在[技术参考](docs/reference.md#本轮验证)与[开发记录](docs/changelog.md)。
本地测试通过不等同于公网部署、容量压测或所有可选编解码后端均已验证。

## 后续拆分方向

按可独立验证的步骤推进：

1. 保持组件接口、依赖和生命周期清晰，支持替换实现。
2. 选择一种处理能力，建立可单独运行的处理进程。
3. 定义任务创建、取消和状态查询 API，并接通媒体订阅与输出发布。
4. 加入任务标识、幂等、超时、节点能力与负载调度，再完善故障恢复。

当前已完成组件接口、工厂注入和构建模块拆分。独立处理进程、远程任务协议和跨进程媒体传输属于后续工作。

## 详细文档

- [技术参考](docs/reference.md)：线程、网络、协议、媒体管理、控制面和详细验证记录。
- [开发记录](docs/changelog.md)：历史变更与对应验证范围。
- [运行指标](monitoring/README.md)：指标口径、采样和 HTTP 查询。
- [日志实现](log/logger.h)：结构化输出、节点身份、有界队列与轮转；[Docker 日志说明](deploy/cluster/README.md#http-核心日志)提供 HTTP 事件和导出步骤。
- [下行分配阅读指南](docs/downlink-allocation-livekit.md)：LiveKit 源码对照、分配阶段与预算移交算例。
- [WebRTC 趋势检测学习指南](docs/webrtc-trendline.md)：固定版本源码、移植边界与算法阅读顺序。
- [媒体质量诊断日志](docs/media-quality-diagnostics.md)：服务端采样、TWCC 协商、字段口径与实测记录。
- [传输实验室](examples/webrtc-room/README.md)：浏览器收发、质量面板与 TWCC 验证步骤。
- [集群部署](deploy/cluster/README.md)：Control/SFU 启动、Docker、公网入口与当前部署边界。
- [只有公网 IP 的浏览器测试](deploy/browser-test/README.md)：通过 SSH 访问测试页面和信令，音视频直连公网 UDP。
- [控制服务与媒体服务的职责边界](docs/control-media-boundaries.md)：状态归属、租约过期、探针语义与真实浏览器故障验证。
- [FFmpeg 源码构建](third/ffmpeg/README.md)：本地编译、codec 组件选择、统一 CMake 接入及录制 / 转码验证。
- [NACK 业务场景与代码阅读](docs/nack-recovery.md)：上行请求、下行重传、批次调度、恢复期限与当前边界。
- [RR 回退实现](media/quality/NetworkController.cpp)：内置比例调整、TWCC 目标接入与 Pacer 输出。
- [会议混流](service/ConferenceMixService/README.md)：任务、后端契约与输出边界。
- [录像合成示例](service/RecordService/examples/README.md)：离线拼画面与混音实验。
- [解包与弱网测试](Common/Depacketizer/Test/README.md)：H264 接收与恢复实验。
