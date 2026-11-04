# Packetia SFU

基于 C++17 的音视频服务学习项目，包含 RTSP、RTMP、SIP 接入、WebRTC 房间转发、
TURN UDP 中继及录像模块。支持原有 `Packetia` 单进程入口，以及 `PacketiaControl` 控制服务和多个
`PacketiaSfu` 节点的独立进程部署；运行方式见[会议控制与节点调度](#会议控制与节点调度)。

当前已验证：两个浏览器加入同一房间，A 发布、B 观看，B 强制经过 TURN UDP；
双方信令意外断开后恢复原参与者，通过 ICE restart 建立新路径并继续传输媒体。

媒体计算层已接入 H264、H265、VP8、VP9、AV1 的软件编解码，支持一路完整编码帧解码一次后生成多档输出；
接收侧补齐 RTP 排序、回绕、缺包等待期限及长断流前向重同步。计算层支持的格式与协议接入范围分别说明，见下文。

### 阅读导航

- [构建与依赖](#构建与依赖)：Linux/macOS 构建、测试依赖。
- [会议控制与节点调度](#会议控制与节点调度)：一控两节点、本地启动、房间接口与 Docker Compose。
- [网络层边界与线程约定](#网络层边界与线程约定)：调度归属、TCP 缓冲、TLS 传输。
- [媒体层结构与命名](#媒体层结构与命名)：目录边界、源轨道描述、发布信息与消费关系。
- [媒体流管理](#统一发布流注册)：core 流注册、轨道归属、RTSP 接入及释放流程。
- [RTP 参数与 SDP 适配](#媒体层结构与命名)：协议参数归属、候选选择及与通用轨道的边界。
- [视频转码与多编码适配](#视频转码与多编码适配)：默认 720p/360p 输出、任务接口、实现选择与验证命令。
- [多编码适配设计](#视频转码与多编码适配)、[业务格式兼容方案](#视频转码与多编码适配)：已实现能力与后续协议、容器、播放接入规划。
- [RTP 接收排序与弱网恢复](#rtp-接收排序与弱网恢复)：等待预算、前向重同步、本机实测与复现。
- [服务端切网恢复框架](#信令恢复与-ice-restart)：核心 C++ 路径状态、媒体适配和带宽估计重置，以及后续接入任务。
- [TURN UDP 中继](#turn-udp-中继)：认证、资源管理、STUN 地址发现与双栈。
- [参与者与连接数据结构](#参与者与连接数据结构)：对象归属、信令恢复与 ICE restart。
- [WebRTC 房间 owner](#webrtc-房间-owner)：固定房间执行器、跨线程信令与共享 UDP、队列和清理。
- [双浏览器房间与 TURN 示例](#双浏览器房间与-turn-示例)：启动方式与浏览器检查。
- [WebRTC SDP 协商](#webrtc-sdp-协商)、[RTP 协议规范](#rtp-协议规范)：协商行为与 RFC 支持边界。
- [房间鉴权](#房间鉴权)：JWT 验证、房间权限与参与者身份绑定。
- [账号与登录框架](#账号与登录框架)、[HTTP 业务接口](#http-业务接口)：账号路由、请求格式、返回契约与实现状态。
- [运行指标与 HTTP 监控](#运行指标与-http-监控)：模块拆分、采样生命周期、访问方式与覆盖范围。
- [弱网与带宽控制](#弱网与带宽控制)、[丢包恢复说明](#弱网与带宽控制)：NACK/SRTP 重传、TWCC/RR、OU 时延检测与码率控制。
- [本轮验证](#本轮验证)、[开发记录](#开发记录)：验证范围与历史变更。

### 当前能力与限制

| 模块 | 当前能力 | 当前限制 |
| --- | --- | --- |
| WebRTC/SFU | WebSocket 信令、H264/Opus 协商、DTLS/SRTP、房间发布与订阅 | 示例每人一个媒体会话，只承担发布或订阅一种角色；动态轨道重协商未开放 |
| ICE | ICE-Lite、认证提名、存活检测、同会话 ICE restart、路径状态通知与发送估计重置 | 主动检查和重新收集候选由浏览器完成；尚未接通 SFU 双栈候选发布；真实移动网络切换待验收 |
| 断线恢复 | 默认 15 秒保留成员、媒体会话和转发关系，凭恢复凭据绑定新信令连接 | 仅同页面、同进程内恢复；刷新页面、服务重启或媒体终止后需重新加入 |
| TURN/STUN | UDP 长期凭证认证、中继、权限、通道、到期清理、配额/ACL、IPv4/IPv6 | 不支持 TURN TCP/TLS、双地址族 Allocation 或完整 coturn 功能 |
| TCP/TLS | 有界 TCP 缓冲、所属线程调度、TLS 1.2/1.3 服务端传输组件 | TLS 尚未接入 TURN 或 WSS 监听、应用配置 |
| 弱网与带宽控制 | NACK 重传、PLI/FIR、TWCC/RR、联合码率估计、按 Transport 分配订阅带宽、视频暂停/关键帧恢复及受限 padding 探测 | 单编码整轨准入；实际 Pacer、编码器调码率、simulcast/SVC 选层与 FEC 尚未接入 |
| RTP 接收排序 | 按 SSRC 隔离、乱序/回绕处理、有界缓存与等待、连续包验证后的前向重同步；H264 独立组帧期限 | 等待预算受 worker 调度影响；不保证端到端时延；后退重启和半周期歧义需会话重置 |
| 视频编解码 | FFmpeg 软件适配 H264/H265/VP8/VP9/AV1，按格式、方向、参数和实现偏好创建实例 | 取决于本机 FFmpeg 实现；当前输出 SDR 8-bit I420；硬件、音频转码尚未接入 |
| 多档转码 | 一次解码、多档缩放编码；默认 H264 → 720p/360p H264；有界输入队列、任务启停与状态统计 | 原 `Packetia` 入口集成；独立 `PacketiaSfu` 尚未接入；缺少任务 HTTP API、派生流 RTP 输出及浏览器播放闭环 |
| 应用鉴权 | HS256 房间 JWT、发布/订阅及成员管理权限、会内权限更新、身份绑定和恢复校验 | 登录与签发由业务后端负责；会内变更不写回控制库，无自动刷新或 JWT 撤销列表 |
| HTTP/账号 API | libwebsockets HTTP 接入、有界业务队列、SQLite 账号与会话、scrypt 密码校验、5 个账号接口 | 单进程开发配置启用本机 API 与注册；控制入口另含房间、成员和入房凭证接口 |
| 运行指标 | Worker 排队/执行耗时及 p95/p99、发送积压/速率/失败、Worker 忙碌率、进程与整机 CPU/内存；HTTP 查询快照 | 尚未统一覆盖 AI、混流和转码自有线程队列；不包含 libwebsockets 与内核发送队列 |
| 多进程部署 | 独立控制进程、多 SFU、节点注册/心跳、持久化房间绑定、节点/实例 JWT 校验 | 单控制服务；支持空房间绑定回收和结束会议，尚无 SFU 故障迁移 |

尚未做系统容量压测，配置中的会话上限不是实际可承载并发数。当前浏览器验证属于本机互通验证，
公网部署仍需配置可达的媒体/中继地址、端口及安全信令入口。

创建会议、入会、音视频发布订阅的实际流程，以及轨道事件通知和动态订阅的当前限制，见[信令与会议流程](#媒体层结构与命名)。

RTSP 支持复用公共鉴权层的 Digest 认证与精确流权限，默认关闭；配置类型见 [RtspAuthOptions.h](service/Auth/RtspAuthOptions.h)，认证实现见 [RtspDigestAuthenticator.cpp](service/Auth/RtspDigestAuthenticator.cpp)。

## 构建与依赖

项目自身的头文件统一使用文件名引用，例如 `#include "SharedBuffer.h"`。
搜索目录由 CMake 配置；新增跨模块引用时，需要在相应目标的
`target_include_directories` 中声明头文件目录。系统库和第三方库保留标准路径，
例如 `<sys/socket.h>`、`<openssl/ssl.h>` 和 `"nlohmann/json.hpp"`。

### Linux 与 FFmpeg 依赖

主程序、录像和转码模块通过 `pkg-config` 查找系统安装的 libavformat、libavcodec、libavutil 和 libswscale
开发包。无需编译 `third/ffmpeg` 下的源码，原有 `PACKETIA_FFMPEG_ROOT` 配置不再使用。
仅安装 `ffmpeg` 命令行程序不够，构建还需要对应的开发头文件和库。

Ubuntu 上安装依赖并构建主程序：

```sh
sudo apt update
sudo apt install build-essential cmake pkg-config libssl-dev \
  libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libsqlite3-dev \
  libwebsockets-dev libsrtp2-dev libcurl4-openssl-dev

cmake -S . -B build -DBUILD_TESTING=OFF
cmake --build build --target Packetia -j"$(nproc)"
```

库必须与目标操作系统和 CPU 架构一致。可通过
`pkg-config --modversion libavformat libavcodec libavutil libswscale` 检查检测到的版本。

转码还要求 FFmpeg 构建包含所选编解码实现。可用 `ffmpeg -encoders`、`ffmpeg -decoders` 辅助检查，
应用以 `VideoCodecFactory::Capabilities()` 和实际初始化结果为准；开发库可链接不代表五种格式都可运行。

### 单进程启动

日常开发可以直接运行单进程入口，无需启动控制服务和多个 SFU：

```sh
./build/Packetia
```

CMake 首次配置会把 `config/packetia.json` 复制为 `build/packetia.json`；后续配置和编译
保留已有文件中的修改。默认配置启用账号服务、本机 HTTP API 和本机自助注册，
地址为 `http://127.0.0.1:8081`，RTSP 使用无需特权的 `8554` 端口。
程序会打印实际配置路径、HTTP 地址和数据库路径；Ctrl+C 会依次停止服务。
账号数据库的父目录由服务自动创建。
日志默认输出到终端和滚动文件，集群脚本会按节点统一收集日志；日志实现见 [logger.h](log/logger.h) 和 [logger.cpp](log/logger.cpp)。

```sh
# 修改配置后重启即可；也可以显式指定其他配置
./build/Packetia --config config/packetia.http-local.json

# 只检查配置，不启动监听或创建数据库
./build/Packetia --check-config
./build/Packetia --help

# 在另一个终端检查 HTTP；未登录时预期返回 401
curl -i http://127.0.0.1:8081/api/v1/me
```

配置查找顺序为 `--config`、`PACKETIA_CONFIG`、可执行文件同目录的 `packetia.json`、
当前目录的 `packetia.json`、当前目录的 `config/packetia.json`；均未指定或找到时使用内置默认值
（HTTP 关闭）。指定的文件缺失或内容错误会直接报错。原有功能环境变量继续覆盖文件中的对应字段。
数据库、录像等相对数据路径仍相对于启动时的工作目录；从项目根目录运行可保持数据位置一致。
本机注册配置用于开发，部署时可修改 HTTP 地址和注册开关。

单进程入口提供账号 API、媒体接入及已配置的本地服务；房间创建、成员管理和入房 JWT 签发
目前属于 `PacketiaControl`。需要这些控制接口或多节点部署时，再使用[多进程启动方式](#会议控制与节点调度)。

WebSocket 默认启用，需要 libwebsockets；WebRTC 媒体运行需要 libsrtp2。
缺少 libsrtp2 时部分协议目标仍能编译，但 WebRTC 服务不能正常启用，浏览器示例和相关测试也不可用。

默认 `BUILD_TESTING=ON`，只有开启时才查找 GoogleTest 并生成测试目标。
Ubuntu 上运行完整默认测试：

```sh
sudo apt install libgtest-dev
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

录像集成测试额外需要 `-DPACKETIA_BUILD_RECORDING_TESTS=ON`，通过 PATH 查找 `ffmpeg`。
生成测试素材需要 libx264 和 AAC 编码支持，测试解码验证需要 Python 3，Ubuntu 可用 `sudo apt install ffmpeg python3` 安装。
录像测试也支持独立配置：

```sh
cmake -S service/RecordService/Test -B build/recording-tests
cmake --build build/recording-tests --parallel
ctest --test-dir build/recording-tests --output-on-failure
```

## 视频转码与多编码适配

转码以完整编码帧为输入和输出，当前调用链如下：

```text
RTP 接收 / 解包 → 完整编码帧 → EncodedFrameRouter
  → TranscodeSession 有界队列 → 任务 worker
  → VideoTranscoder：解码一次 → 每档缩放、编码
  → TranscodedFrameRouter → 应用消费者
```

现有 RTP 视频组帧路径为 H264。其他格式可从完整编码帧接口接入计算层，
仍需分别补齐对应的 RTP 解包、封包、会话协商和容器适配，才能用于相应业务。

| 格式 | FFmpeg 解码候选 | FFmpeg 编码候选 |
| --- | --- | --- |
| H264 | `h264` | `libx264` |
| H265 | `hevc` | `libx265` |
| VP8 | `vp8`、`libvpx` | `libvpx` |
| VP9 | `vp9`、`libvpx-vp9` | `libvpx-vp9` |
| AV1 | `libdav1d`、`libaom-av1` | `libsvtav1`、`libaom-av1` |

每档可指定格式、分辨率、bps 和实现偏好。工厂支持 `Auto`、`Prefer`、`Require`：
自动选择、优先指定实现、必须使用指定实现；回退仅限同一编码格式，失败返回具体候选及原因。
本机已验证 x264、x265、libvpx、SVT-AV1 和 dav1d 路径，libaom 候选尚未实测。
缩放保持显示宽高比并补黑边；硬件后端、10-bit/HDR 和 SVC 不在当前交付范围。

已启动的 `ServerApp` 可以用下面的 C++ 调用创建默认任务。身份值需要替换成实际输入来源及独立输出身份：

```cpp
service::transcode::TranscodeTaskConfig task;
task.task_id = "camera-720p-360p";
task.input = {10, 42};                       // 实际 endpoint_id、track_id
task.output = {20, "session", "camera"};     // 与输入不同的输出身份
// 默认 H264 输入，输出 1280×720 / 2 Mbps 和 640×360 / 600 kbps 两档 H264。
// 默认任务按原始 90 kHz RTP32 时间戳处理；已展开的媒体时间须改为 MediaTime。
if (!app.StartTranscode(task)) {
    auto error = app.TranscodeLastError();
    // 将 error 交给应用的错误处理入口。
}
// app.TranscodeStatus(task.task_id)：状态、队列、处理/丢弃/恢复计数。
// app.TranscodedFrameRouter()：由应用订阅输出，流名为 camera/720p、camera/360p。
// app.StopTranscode(task.task_id)：停止并回收任务。
```

输入回调只向默认 32 帧、16 MiB 的有界队列提交帧。每个活动任务由一个 `ManagedThread`
串行执行编解码；溢出后等待随机访问关键帧并重建解码状态。没有任务时不创建转码 worker。
这些是容量限制，目前尚无转码输入帧的等待期限，也未将带宽估计接入运行时编码码率控制。

当前服务在原 `Packetia` 入口注册；独立 `PacketiaSfu` 尚未集成。
应用需显式订阅输出，尚无转码任务 HTTP API 或派生流 WebRTC 发布闭环。
更多参数、所有权、线程契约和格式边界见[多编码适配设计](#视频转码与多编码适配)，
各业务的透传、换封装和转码规划见[业务格式兼容方案](#视频转码与多编码适配)。

使用已配置 `BUILD_TESTING=ON` 的构建目录运行转码测试：

```sh
cmake --build build --target test_transcode_domain test_video_transcoder test_multicodec -j4
ctest --test-dir build --output-on-failure \
  -R '^(transcode_domain|video_transcoder|multicodec)$'
```

## 各路独立录像切片

各路独立 fMP4 切片、SQLite 索引及查询接口见 [RecordingService](service/RecordService/RecordingService.h)
和 [RecordingCatalog](service/RecordService/RecordingCatalog.h)。
按 `(session_id, stream_id)` 分别录制，在关键帧处按媒体时间切段；支持逐流停止、重启，以及按会话、流和时间范围查询完成片段。构建需要 SQLite 3.24 以上开发库。

## 录像画面合成实验

可通过[双人合成示例](service/RecordService/examples/README.md)体验两路视频左右拼接和音频混音，
支持生成测试画面或输入两份已完成的录像，输出单路 H264/AAC MP4。

## 会议合成框架

[ConferenceMixService](service/ConferenceMixService/README.md) 提供会议输入配置、独立工作线程、任务启停与编码输出接口。
当前尚未实现实际拼画面、混音和编解码。服务已注册到统一启停管理，默认关闭。

录制、AI、会议合成支持全局开关、流默认值与逐流覆盖配置，字段见
[AppConfig](config/AppConfig.h) 和 [配置示例](config/packetia.example.json)，统一启停入口见
[ServerApp](server/ServerApp.cpp)。

## macOS 编译

需要 Xcode Command Line Tools（`xcode-select --install`）及 Homebrew。
在项目根目录执行：

```sh
brew install cmake ninja pkgconf googletest openssl@3 ffmpeg sqlite libwebsockets libsrtp

cmake -S . -B build/macos -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH="$(brew --prefix)" \
  -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)" \
  -DPACKETIA_LIBWEBSOCKETS_ROOT="$(brew --prefix libwebsockets)"
cmake --build build/macos --parallel "$(sysctl -n hw.ncpu)"
ctest --test-dir build/macos --output-on-failure
```

只构建主程序可以在构建命令中加入 `--target Packetia`，产物为
`build/macos/Packetia`。完整构建也会生成测试程序；只构建主程序后不要直接运行全量 CTest。

macOS 使用 kqueue 事件调度，Linux 使用 epoll。依赖库必须与编译架构一致；
Apple Silicon 建议使用原生 arm64 Homebrew，避免混用 Rosetta 的 x86_64 库。
`third/libwebsocket` 中预编译的 Linux 库不用于 macOS。

服务默认监听 RTSP 554、SIP 5060、RTMP 1935、UDP 9000 和 WebSocket 8080，
启动前请确认这些端口可用。默认配置见 `config/AppConfig.h`；`server/ServerConfig.h` 是类型别名。

## 网络层边界与线程约定

网络接入继续由一个 `Packetia` 进程管理。`ServerApp` 负责服务配置、启动回滚和停止，
`EventLoop` 管理 I/O 线程，每个网络对象固定归属一个 `TaskScheduler`。

| 层次 | 现有组件 | 边界 |
| --- | --- | --- |
| 事件调度 | `EventLoop`、`TaskScheduler`、`Channel` | 调度线程、事件注册、任务和定时器 |
| socket 与连接 | `TcpSocket`、`UdpSocket`、`Acceptor`、`TcpConnection`、`UdpServer` | 系统收发、监听、缓冲和连接关闭 |
| 传输适配 | `IDatagramTransport`、`UdpDatagramTransport`、`TlsTransport` | 数据报接口、独立存储及 TCP 上的 TLS 明文适配 |
| 协议 | TURN、RTSP、SIP、RTMP、WebRTC | 拆包、认证、会话、权限和媒体路由 |

### 线程归属

线程创建统一经过 [`utils::ThreadManager`](utils/ThreadManager.h)，由 `ManagedThread` 持有线程、
发出协作停止请求并等待退出。I/O、命名工作线程池、转码任务、AI 帧处理、会议混流和异步日志均已接入；
主线程通过 `TrackCurrent` 登记。线程状态包括模型、分组、名称、原生线程 ID、启动时间和未捕获异常。
已退出但尚未回收的线程仍可查询，`Join` 完成后移除登记。FFmpeg 等第三方库内部线程由对应库管理。

```text
ServerApp / ServerLauncher（按依赖顺序启停）
  ├─ EventLoop → I/O 事件循环
  ├─ WorkerRegistry → media / endpoint / transcode / API / recording
  ├─ TranscodeService → 每个活动任务一个 worker
  └─ AI / mix / logging → 专用循环
                  ↓
        ThreadManager / ManagedThread（统一创建、命名、停止请求、回收、状态登记）
```

`ServerApp::ThreadStatus()` 返回进程内已登记线程、命名池的线程数/队列深度/字节数/丢弃数，
以及本应用 I/O 调度器的运行状态、等待任务数、执行数和拒绝数。这些是并发采样值，
不是整个进程同一时刻的原子快照；I/O 任务计数只统计 `Post`/排队 `Invoke`，不代表网络包数量。

启动配置保留 `server.io_threads`、`account.worker_count`、`recording.worker_count`，
新增 `workers` 配置媒体、Endpoint、转码池的线程数和每个分片的队列长度：

```json
"workers": {
  "media_threads": 4,
  "endpoint_threads": 4,
  "transcode_threads": 0,
  "media_queue_len": 4096,
  "endpoint_queue_len": 4096,
  "transcode_queue_len": 512
}
```

线程数支持 1～64，转码的 `0` 表示按 CPU 数自动选择 1～4；配置在启动时生效，
不支持运行中直接修改分片数量。各用途仍使用独立队列，媒体和 Endpoint 保留丢弃最旧任务策略，
API/转码使用拒绝新任务策略。旧 `QueueManager` 已复用有界命名池，新增 `Stop` 和可检查拒绝的 `TryPost`。

这里的 `workers.transcode_threads` 和 `transcode_queue_len` 配置命名转码池，
新 `TranscodeService` 的任务 worker 使用独立线程与帧队列，不受这两个字段控制。
接收流的约 10 ms 超时检查复用所属媒体 worker，不为每条流创建线程。

整机停机使用 `ServerApp::Stop()`：先关闭接入，再排空工作队列和帧消费者，最后回收 I/O 线程。
单个服务的停机使用其 `Stop`；`ThreadManager::RequestStop(id)` 只是底层协作请求，
不会关闭监听器、清理房间或完成服务状态迁移。专用循环必须提供停止及唤醒回调，
并由控制线程完成 `Join`；在自身线程上请求停止后，仍需保留句柄让控制线程回收。
日志线程保持到日志器析构，以记录业务与网络停机过程。

- `EventLoop::GetTaskScheduler()` 是轮询分配入口，应在创建或启动对象时选择并保存结果。
  `EventLoop` 的任务、定时器和 Channel 便捷接口固定操作第一个调度器，不代表当前对象的归属。
- `TcpServer`、`Acceptor` 和 `UdpServer` 都支持注入 `shared_ptr<TaskScheduler>`。
  注入模式下停止、重新监听仍使用同一调度器；旧调度器停止后，应使用新调度器创建新对象。
  兼容的 `EventLoop*` 构造方式仍在每次启动时选择调度器。
- TCP 监听器及其连接使用同一调度线程；当前 TURN 控制入口、会话、relay socket 和定时器
  也使用同一调度线程。WebRTC 另外按房间固定分配 owner；实际 I/O 调度器为 3 个时，
  使用 1 个网络执行器和 2 个房间 owner，详见[房间 owner](#webrtc-房间-owner)。
- 读取、协议状态变更、关闭通知在对象所属线程执行。TCP 回调设置通过 `Invoke` 串行化；
  启停和回调设置属于控制操作，调用方需串行安排。发送接口另有下述并发约定。
- 跨线程提交业务命令使用 `Post` 并检查是否接受；数据库、远程鉴权及文件操作交给工作线程，
  完成后回到所属调度线程检查会话是否仍然有效。I/O 线程之间避免同步 `Invoke` 相互等待。
  调度器停止后的 `Invoke` 只用于最终清理；此时清理可在调用线程执行。

### TCP 启动、缓冲与关闭

`TcpServer::OnConnect` 构造连接并安装协议回调；服务随后登记连接并安装断开清理回调，
再调用 `OnConnected` 完成协议初始化，最后统一 `Start` 注册读取事件。
RTMP 的 transport 初始化位于 `OnConnected`，其内部 `Start` 仍然是幂等操作。
初始化期间断连也能移除已登记连接。协议派生类析构时须先 `Stop`，保证回调所引用的状态仍有效。

`TcpConnection::Options` 可按协议指定以下限制，零值使用默认值：

| 配置 | 默认值 | 含义 |
| --- | --- | --- |
| `max_receive_bytes` | 1 MiB | 连接未消费字节及接收缓存大小上限 |
| `max_send_bytes` | 4 MiB | 本地发送队列字节上限 |
| `read_budget_bytes` | 128 KiB | 单次可读回调从 socket 读取的字节预算 |

接收缓存复用已消费空间，满时先让协议解析；若协议保留整个满缓存且没有安排解析续调，
则关闭连接。`BufferReader::Read` 的容量耗尽返回 `-1/EMSGSIZE`，与返回 `0` 的 EOF 分开。
读取预算耗尽后由水平触发的 epoll/kqueue 在后续轮次继续读取。
协议自己的累积缓存、消息大小和每轮解析数量仍需独立限制，连接限额不覆盖协议复制出去的数据。
`RequestReadContinuation` 用于分批消费已经缓存在连接中的字节，提交失败必须由调用方处理。

`ReadCallback` 和 `BytesCallback` 为两种互斥模式，最后设置的模式生效。
前者由协议显式消费 `BufferReader`；后者在回调返回后自动消费该批字节。
传入指针和缓冲区均为临时借用，跨线程或延迟处理需要复制或持有独立存储。

`TcpConnection::Send` 支持跨线程、有界入队；`Queued` 只表示进入本地队列。
`UdpServer::TrySendTo` 在所属线程即时发送；`Sent` 只表示内核接受，`NotWritable` 不会自动排队。
两者均不保证对端收到；UDP 跨线程调用当前会同步等待所属线程完成，媒体路径应尽量同线程调用。
队列满后的拒绝、暂停或关闭策略由协议决定，TCP 不可任意丢弃已经发送了一部分的消息。

连接关闭只通知一次，通知期间不持有发送锁；对端半关闭时先完成已安排的解析与排队回复，
再释放连接。UDP 会话清理与共享监听 socket 关闭分别管理。
进程停止时先停止接入、清理协议与网络对象，再停止 I/O 线程；网络对象也保留停机后清理能力。

### TLS 服务端传输

`network/transport/TlsContext` 加载 PEM 证书链和匹配的未加密私钥，缺失、无效、不匹配或加密私钥
都会返回失败，加载过程不会交互询问密码。构造完成后不再修改配置，可由多个连接共享。
最低版本为 TLS 1.2，同时支持 TLS 1.3；使用 OpenSSL 默认密码套件，禁用压缩和重新协商。
服务器目前不要求客户端证书，应用认证仍由上层协议负责。

`TlsTransport` 包装已接受的 `TcpConnection`，使用 OpenSSL 内存 BIO，并复用连接所属调度器。
它独占该连接的字节、关闭和发送排空回调，保留 `TcpServer` 的断开清理回调。
在 `OnConnected` 中安装 transport，协议会话应保存它的 `shared_ptr`，握手成功后再处理协议业务。
同一个连接不可同时安装明文协议的 TCP 读取回调。

```cpp
using network::transport::TlsContext;
using network::transport::TlsTransport;

std::string error;
auto context = TlsContext::CreateServer({certificate_path, private_key_path}, &error);
// 服务启动时检查 context；每个连接复用该配置。
auto tls = std::make_shared<TlsTransport>(connection, context);
TlsTransport::Callbacks callbacks;
callbacks.on_ready = [] { /* 握手完成 */ };
callbacks.on_bytes = [](const uint8_t* data, size_t size) { /* 协议累积并拆包 */ };
callbacks.on_closed = [](TlsTransport::Error reason) { /* 清理协议会话 */ };
bool started = tls->Start(std::move(callbacks));
```

接收链路为 `TcpConnection -> 输入 BIO -> SSL_read -> on_bytes`；发送为
`TlsTransport::Send -> 明文队列 -> SSL_write -> 输出 BIO -> TcpConnection::Send`。
TLS 明文仍是字节流，回调可能包含半条或多条协议消息；数据只在回调期间有效。
所有 SSL 操作、定时器和回调在连接所属线程执行。
`Send` 复制数据并有界入队，跨线程调用同步等待所属线程处理；`Queued` 不保证对端收到，
握手未完成或明文队列已满返回 `NotWritable`。TLS 内部每轮最多处理 32 次读取和 32 次加密写入，
剩余工作通过调度任务继续执行。

TCP 队列满时，未接受的密文保留在输出 BIO；TCP 排空通知触发重试，仅在 TCP 接受后消费密文。
默认握手超时 10 秒、对端 TLS 关闭响应超时 2 秒、待加密明文上限 1 MiB、输入和输出 BIO
未消费字节各 256 KiB，均可通过 `TlsTransport::Options` 配置非零值。
这些限制不包含 TCP 自身队列、协议累积缓存、OpenSSL 内部记录存储或分配器开销。

状态为 `Created -> Handshaking -> Open`，失败进入 `Failed`。
对端 `close_notify` 进入 `Closing`，放弃尚未加密的明文，发送 TLS 关闭响应并排空 TCP 队列后
进入 `Closed`；未经 TLS 关闭的 TCP 断连单独报告 `TcpClosed`。
`Close()` 是立即取消，放弃排队数据并断开 TCP，报告 `LocalClose`，不执行主动优雅 TLS 关闭。
关闭通知最多一次；析构取消定时器、断开连接，不调用上层关闭回调。
客户端负责验证服务器证书链和域名，公网浏览器需使用可信证书。

验证：

```sh
cmake --build build/webrtc-integration --target test_tls_transport -j 4
ctest --test-dir build/webrtc-integration --output-on-failure -R '^test_tls_transport$'
```

测试使用真实 OpenSSL 客户端覆盖 TLS 1.2/1.3、分片握手、二进制回显、微小 TCP 队列背压、
分批发送、证书加载与信任校验、握手超时、缓冲限额、TLS/TCP 断连和资源清理。

### 后续 TURN TCP/TLS 接入

当前 TURN 仍仅支持 UDP，TCP socket 仍为 IPv4；尚未增加 TURN TCP/TLS 监听或会话分片。
TLS 服务端适配已实现，但未接入应用配置或 TURN 监听。下一阶段统一 TCP 地址族和协议无关的
字节流接口，再增加 TURN 流拆包与连接身份，复用 `TlsTransport`。
TLS 负责握手和加解密，TURN 负责 STUN/ChannelData 消息边界和业务状态。
WebRTC DTLS 保持数据报边界；WebSocket 当前通过 libwebsockets 适配所属调度器。

## 账号与登录框架

`service/Account` 与房间鉴权 `service/Auth` 并列：前者定义用户、登录会话、存储和密码校验接口，
后者验证入房 JWT、检查媒体操作权限。`server/api` 提供账号路由和独立有界工作队列，
`protocol/http` 定义 HTTP 适配与异步回复约定。原入口由 `ServerApp` 管理生命周期，
分布式入口由 `ControlApp` 装配业务与节点内部两个独立 HTTP 监听。
HTTP 协议层独立构建为 `packetia_http`，底层 socket/TLS 传输仍位于 `network`。
适配器复用项目已有的第三方库 libwebsockets，由库处理 HTTP 协议，
`server/api` 负责业务路由与调度。HTTP 已支持监听、分段请求体、Keep-Alive、HEAD、
工作线程异步回复、连接/请求/响应缓冲限制和超时；所有库操作固定在所属 I/O 调度器执行。
停止监听会清理连接并使迟到回复失效，停止或 EventLoop 重启后可重新启动。
目前面向 Content-Length 请求的 HTTP/1.1 API，拒绝 Transfer-Encoding 和协议升级；
HTTPS 证书配置、CORS 和性能压测尚未完成。实现入口见 [HttpServer](protocol/http/HttpServer.h)，
服务接口见 [IHttpServer](protocol/http/IHttpServer.h)。

API 采用 ZLMediaKit `WebApi.cpp` 的路由注册、参数适配和统一回复思路。
[ApiRouter](server/api/ApiRouter.h) 按 HTTP 方法与路径精确匹配；各业务模块通过
`RegisterRoutes` 注册独立处理函数，再由 `ApiServer` 在启动时构建只读路由快照。
新增接口在所属模块注册。`ApiServer` 支持注入已构建的路由，`ControlApp` 装配账号与房间路由；
原账号构造方式仍由 `ApiServer.cpp` 的 `BuildRoutes` 装配。
当前账号注册入口见 [AccountController](server/api/AccountController.cpp)：

```cpp
router.PostJson("/api/v1/sessions", [controller](const ApiRequest& request) {
    return controller.Login(request);
});
router.Get("/api/v1/me", SessionRoute(accounts_, GetCurrentUser));
```

`PostJson` 统一检查 Content-Type、请求体上限、JSON 对象、重复键和嵌套深度；
处理函数直接读取 `request.json`，字段和权限由所属业务模块验证。
公共 JSON/错误响应位于 `ApiResponse`，路由层统一处理 404、405（含 Allow）和未捕获异常。
会话保护路由共用 `SessionRoute` 凭据检查；自助注册受 `account.registration_enabled` 控制，关闭时返回 403。
重复注册同一方法与路径会报错；运行期间只读查表，处理函数在现有有界业务线程池执行，
HTTP 异步回复仍返回所属 I/O owner。当前路由处理函数同步返回结果，原生异步业务处理器、
动态路径参数和查询参数解析尚未扩展。注册方式参考
[ZLM WebApi.cpp](https://github.com/ZLMediaKit/ZLMediaKit/blob/master/server/WebApi.cpp)。

账号业务借鉴 [Ory Kratos](https://github.com/ory/kratos) 的身份、密码凭据和服务端会话职责划分，
在 C++ 中实现，不依赖独立 Kratos 服务，也不兼容其 self-service flow API。
`SqliteAccountStore` 分表保存用户、密码凭据与会话；`ScryptPasswordHasher` 使用 OpenSSL scrypt，
会话使用随机 Bearer token，仅保存 SHA-256 摘要。续期通过事务撤销旧 token 并创建新会话。
内置默认值及 `packetia.example.json` 中的 `account.enabled`、`http.enabled`、
`account.registration_enabled` 均关闭；直接启动时自动加载的 `packetia.json` 为本机开发启用这三个开关。
示例配置见 `config/packetia.example.json`；多进程账号、成员和入房流程见[会议控制与节点调度](#会议控制与节点调度)。
账号 API 的登录会话与房间 JWT 用途不同；邀请注册、密码找回、MFA、登录限流及 Cookie/CSRF 流程尚未提供。
当前 HTTP 接入未实现原生 HTTPS，公网部署需要在 TLS 反向代理后运行。

### HTTP 业务接口

当前仅注册以下 5 个账号接口，入口为
[`AccountController::RegisterRoutes`](server/api/AccountController.cpp)。
以下接口已接入真实账号业务。注册只创建用户，不自动登录；每次登录创建独立会话，退出仅撤销指定会话。

| 方法 | 路径 | 用途 | 请求内容 | 成功结果 |
| --- | --- | --- | --- | --- |
| `POST` | `/api/v1/accounts` | 注册账号 | JSON：`login`、`password`，可选 `display_name` | `201`：`user` 对象，包含 `id`、`login`、`display_name`、`status`、`created_at_unix_seconds` |
| `POST` | `/api/v1/sessions` | 登录并获取会话凭据 | JSON：`login`、`password` | `200`：用户身份、`session_token`、过期时间 |
| `GET` | `/api/v1/me` | 查询当前登录用户 | Bearer 凭据，无请求体 | `200`：`user_id`、`session_id`、`display_name` |
| `POST` | `/api/v1/sessions/renew` | 续期并轮换登录凭据 | Bearer 凭据，无请求体 | `200`：用户身份、新凭据和过期时间 |
| `DELETE` | `/api/v1/sessions/current` | 注销当前登录会话 | Bearer 凭据，无请求体 | `204`，无响应体 |

注册与登录要求 `Content-Type: application/json`，请求体必须是 JSON 对象，
不接受重复键、未知业务字段或超出 8 层的 JSON 嵌套。
`login` 输入最多 256 字节，去除首尾 ASCII 空格并转为小写后，至少 3 个字符，
仅允许 ASCII 字母、数字及 `._-@+`，唯一性由数据库约束保证。
注册密码为 12..4096 字节，登录密码为非空字符串且最多 4096 字节，不修剪或修改密码。
`display_name` 可省略，默认使用规范化登录名；提供时最多 256 字节，要求有效 UTF-8 且不含控制字符。
以上字符串均不接受内嵌 NUL。
整个请求体还受 `http.max_body_bytes` 限制，默认 8192 字节。

本地联调时，在配置文件中设置 `account.enabled=true`、`account.registration_enabled=true`、
`http.enabled=true`，然后通过 `Packetia --config /path/to/config.json` 启动程序；
直接运行 `./build/Packetia` 使用自动加载的本机开发配置时，这些开关已启用。
默认 HTTP 地址为 `127.0.0.1:8081`，可以用以下命令完成注册和登录。

```sh
# 注册
curl -i http://127.0.0.1:8081/api/v1/accounts \
  -H 'Content-Type: application/json' \
  --data '{"login":"zhangsan","password":"example-password","display_name":"张三"}'

# 登录
curl -i http://127.0.0.1:8081/api/v1/sessions \
  -H 'Content-Type: application/json' \
  --data '{"login":"zhangsan","password":"example-password"}'

# 将 <session_token> 替换为登录返回的凭据
curl -i http://127.0.0.1:8081/api/v1/me \
  -H 'Authorization: Bearer <session_token>'
curl -i -X POST http://127.0.0.1:8081/api/v1/sessions/renew \
  -H 'Authorization: Bearer <session_token>'
curl -i -X DELETE http://127.0.0.1:8081/api/v1/sessions/current \
  -H 'Authorization: Bearer <session_token>'
```

登录与续期成功响应结构如下；过期时间为 Unix 秒。续期在事务中轮换凭据，
成功后客户端必须保存新凭据；旧凭据立即失效，后续查询或重复续期返回 401，退出应使用新凭据。

```json
{
  "identity": {
    "user_id": "user-123",
    "session_id": "session-456",
    "display_name": "张三"
  },
  "session_token": "<session_token>",
  "expires_at_unix_seconds": 1800000000
}
```

`GET /api/v1/me` 成功时直接返回上述 `identity` 对象，不包含凭据。
JSON 响应带 `Cache-Control: no-store`；错误统一使用以下结构：

```json
{
  "error": {
    "code": "unauthenticated",
    "message": "Authentication required or credentials invalid."
  }
}
```

| HTTP 状态码 | 当前处理含义 |
| --- | --- |
| `400` | JSON、字段类型、字段内容或空请求体要求不符合接口约定 |
| `401` | 缺少或无效的 Bearer 凭据；账号业务也将凭据错误、禁用、过期或撤销统一映射到此状态 |
| `403` | 自助注册关闭（`registration_disabled`） |
| `404` | 路径没有注册 |
| `405` | 路径存在但方法不匹配，响应带 `Allow` |
| `409` | 规范化登录名已存在 |
| `413` | 请求体超过上限 |
| `415` | JSON 接口未使用 `application/json` |
| `500` | 业务内部错误或处理函数异常 |
| `503` | 账号服务不可用、API 停止接入或业务队列超过限额 |

`401` 响应同时带 `WWW-Authenticate: Bearer`。登录会话凭据与 WebRTC 房间 JWT
各自用于对应入口：原 `Packetia` 入口保留账号接口；`PacketiaControl` 另提供 HTTP 房间创建、
房主添加成员和入房 JWT 签发，见[多进程运行说明](#会议控制与节点调度)。线程状态当前通过 C++ 的
`ServerApp::ThreadStatus()` 查询；Worker 和资源指标通过下述 HTTP 监控接口查询。

账号登录、控制入口的房间成员准入与入房凭证签发已实现；尚无成员查询和完整会议管理接口。

## 运行指标与 HTTP 监控

监控按指标计算、系统资源采集、服务生命周期和 HTTP 展示拆分，各层使用独立 CMake 目标：

| 目录 / 构建目标 | 职责 |
| --- | --- |
| `monitoring/core` / `packetia_monitoring` | 指标句柄、计数、直方图、注册表与结构化快照；仅依赖标准库及线程库 |
| `monitoring/collectors` / `packetia_resource_collector` | 读取操作系统 CPU、内存，计算采样差值；不依赖 HTTP 或 JSON |
| `service/MonitorService` / `packetia_monitor_service` | 实现 `IService`，通过 `ThreadManager` 管理采样线程、健康状态和只读快照发布 |
| `server/api/MetricsController`、`MetricsJsonEncoder` / `packetia_monitoring_api` | 注册指标路由、编码 JSON，读取监控服务的缓存快照 |

通用 `packetia_api` 负责路由和有界请求调度，账号接口由 `packetia_account_api` 单独提供，
监控接口不依赖账号服务。应用入口显式组装路由。

`ServerApp`、`ControlApp`、`SfuApp` 各自持有指标上下文与监控服务。
为适配现有全局 Worker 和深层 socket 调用，进程保留一个有作用域的指标注册表绑定：
启动时激活，停止或启动回滚时解除，同一进程同时只允许一个活动应用上下文。
监控服务先于业务生产者启动、晚于生产者停止，默认每秒采样并原子发布只读快照。
HTTP 查询不现场读取系统资源或获取生产者指标锁；HTTP I/O 仍复用已有 EventLoop。
服务不可用时返回 `503`，运行中采样失败保留上次快照，通过 `monitor` 字段报告健康状态和快照年龄。

单进程入口启用 `http.enabled` 后，可在配置的 HTTP 地址查询；默认开发配置为：

```sh
curl http://127.0.0.1:8081/metrics
curl http://127.0.0.1:8081/api/v1/metrics
```

两个 GET 路由返回同一 JSON 结构，无需登录，响应禁止缓存。`PacketiaControl` 在公共 HTTP 监听提供相同路由。
独立 `PacketiaSfu` 通过 `metrics_http_port` 和 `metrics_listen_ip` 配置指标监听；未配置端口时不监听。
HTTP 后端需要构建时启用 `PACKETIA_WITH_LIBWEBSOCKETS`。

当前覆盖 `ShardedWorkerPool`、TCP 发送缓冲、UDP 发送和 TLS 缓冲。
p95/p99 是 Worker 生命周期累计直方图的近似值，速率与忙碌率采用近 5 秒窗口；
Worker 忙碌率包含任务内锁等待，不等同于线程 CPU 使用率；发送成功表示内核 socket 接受数据。
本次拆分保留既有采集范围，尚未统一接入 AI、混流、转码自有线程队列。
完整字段口径、SFU 配置示例和独立测试命令见 [monitoring 模块文档](monitoring/README.md)。

## 会议控制与节点调度

会议业务和集群调度分层实现，借鉴 LiveKit 的控制面与房间运行时分工，使用现有 C++ HTTP、
SQLite、EventLoop 和有界通道，不引入 Redis 或跨节点媒体迁移。

| 模块 | 负责内容 | 状态归属 |
| --- | --- | --- |
| `service/meeting/MeetingModule` | 继承 `ServiceBase`，统一会议控制面的初始化、准入、排空和资源释放 | 注册到 `ServiceManager`，类型为 `ServiceType::Meeting` |
| `service/meeting/MeetingService` | 创建会议、房主校验、持久成员角色、结束状态 | 共享 `ClusterStore` 的 SQLite 事务 |
| `service/meeting/RoomAdmissionService` | 检查成员角色、申请节点、签发入房 JWT | 不持有在线成员 |
| `service/meeting/RoomAdminService` | 校验持久房主，将管理命令发往目标节点 | 注入 `RoomCommandSender`，不在事务中等待网络 |
| `service/Cluster/Coordinator` | 节点注册、续租、实例隔离、房间分配与回收 | 节点租约和带 `assignment_id` 的绑定 |
| `server/meeting/RoomController` | 账号会话认证与公共会议 API | 控制进程 API worker |
| `server/cluster/NodeController` | 节点注册、心跳、带代次的释放确认 | 控制进程内部 API worker |
| `server/meeting/RoomControlController` | 节点密钥、进程实例与租约检查 | SFU 内部 API worker |
| `RoomSignalHandler` | 实际成员查询、修改、踢人、结束和媒体清理 | 房间固定 owner 执行器 |

### 统一服务生命周期

`MeetingModule : ServiceBase` 是会议控制面的应用入口，内部组合 `MeetingService`、
`RoomAdmissionService`、`RoomAdminService` 和共享数据库的 `Coordinator`。
这些业务对象保持原有职责，HTTP 路由只持有模块，不直接访问其内部对象。

```text
ControlApp → ServiceManager
              ├─ MonitorService : IService
              └─ MeetingModule : ServiceBase → IService
                   ├─ ClusterStore / Coordinator
                   ├─ MeetingService
                   ├─ RoomAdmissionService
                   └─ RoomAdminService
```

- `Init`：验证配置、打开数据库并构造业务对象；初始化失败自动回滚本轮资源。
- `Start`：开启操作准入，状态进入 `Running`。`Stop` 后再次 `Start` 可复用已初始化的资源。
- `Stop`：状态进入 `Stopping`，拒绝新操作，等待已接收的数据库或节点命令完成，再进入 `Stopped`。
- `Shutdown`：停止操作后释放业务对象和数据库，回到 `Created`；重新运行需再次 `Init`。
- `State` / `Health`：统一报告模块状态与初始化错误；公共 `/healthz` 同时返回会议模块的状态和错误码。

`ServiceBase` 和 `ServiceManager` 编译到独立的 `packetia_service_core`，不依赖媒体库。
生命周期方法在管理线程调用；业务操作继续在 API worker 上执行。操作计数的锁只用于准入和排空，
执行数据库或 HTTP 请求时释放，不串行化全部业务调用。

控制进程停止时先关闭模块准入并等待已开始的操作完成，然后停止 HTTP 接入、排空 HTTP worker，
再停止账号和 EventLoop，最后调用 `ShutdownAll` 释放资源。旧路由即使继续持有模块，也只能收到
`meeting_unavailable`（503），不会继续操作已关闭的数据库。该生命周期接口用于进程管理线程，
不在网络 EventLoop 或房间 owner 上同步等待。

以后台修改成员为例，完整链路为：

```text
POST /api/v1/rooms/participants/update
  → RoomController：账号 session → MeetingModule：运行状态检查与操作计数
  → RoomAdminService：持久房主校验
  → Coordinator::FindRoom：读取 node_id / instance_id / assignment_id
  → NodeCommandClient：HTTP POST 目标 SFU /internal/v1/rooms/command
  → RoomControlController → WebRtcService::ControlRoom
  → 有界 RoomControlChannel → 固定房间 owner → RoomSignalHandler::ApplyParticipantCommand
  → Room::UpdateParticipant → 成员事件与通知
  → promise 返回真实执行结果 → SFU HTTP 响应 → 控制进程 HTTP 响应
```

SQLite 的锁只覆盖事务；网络 EventLoop 负责接入和回包，HTTP 调用与同步结果等待位于 API worker。
SFU owner 只执行本房间状态变更，不等待 HTTP；房间通知继续经既有信令响应通道回连接执行器发送。
独立 SFU 使用 1 个网络执行器和 2 个房间 owner，媒体 worker 保持独立。

### 公共接口与权限

以下接口均为 POST，使用账号登录返回的 `Authorization: Bearer <session_token>`。
公共后台管理当前要求数据库中的会议房主；WebSocket `roomAdmin` 是会内管理权限，两者分别检查。
房主无需先建立 WebSocket 连接就能调用后台接口。

| 路径 | 请求字段 | 行为 |
| --- | --- | --- |
| `/api/v1/rooms` | `{}` | 创建持久会议，返回 `room_id`，尚不创建媒体连接 |
| `/api/v1/room-members` | `room_id`, `user_id`, `role` | 设置 `speaker` / `listener`，返回 `effective: next_join` |
| `/api/v1/room-admissions` | `room_id` | 校验成员、分配节点、签发 60 秒 JWT，返回信令 URL 和三种路由标识 |
| `/api/v1/rooms/participants` | `room_id`；可选 `limit`, `cursor`, `revision` | 有界成员分页，未分配节点的会议返回空列表 |
| `/api/v1/rooms/participant` | `room_id`, `participant_id` | 查询指定在线或恢复等待中的成员 |
| `/api/v1/rooms/participants/update` | `room_id`, `participant_id`；资料及 `permissions` 补丁 | 更新姓名、元数据、属性及会内权限，复用 WebSocket 的校验和媒体撤权清理 |
| `/api/v1/rooms/participants/remove` | `room_id`, `participant_id` | 踢人，清理媒体和恢复凭据，通知在线连接 |
| `/api/v1/rooms/end` | `room_id` | 停止准入、结束在线房间并回收节点容量，可重试 |

持久角色决定**下一次新 join** 的初始权限；会内 `permissions` 更新只修改当前成员，立即生效。
`resume` 保留原成员的会内有效权限，即使新的入房 JWT 因持久角色变更而带有不同 grants，
也不会覆盖管理员提交的权限或重建成员。需要同时修改长期角色与当前权限时，调用各自接口。
踢人只移除当前参与者，不删除持久成员资格；会议结束后该会议不能再签发入房凭证。

### 房间绑定与回收

三个标识各有用途：`node_id` 定位节点，`instance_id` 隔离节点进程重启，`assignment_id` 隔离同房间的新旧绑定。
节点 JWT 和管理命令均携带这些信息；SFU 只接纳本节点、本进程和未退役的房间代次。

- 准入预留绑定 90 秒，后续准入延长预留时间。心跳每轮返回至多 16 个回收候选；
  节点必须先在 owner 上确认无成员（包括等待恢复的成员），封住旧代次后再向控制面确认释放。
- 最后一人离开或被踢后，owner 退役该代次，由心跳报告释放。控制面收到确认后清除绑定，下一次准入生成新代次。
  回收窗口内若拿到旧凭证而收到 `stale_assignment`，应重新申请准入，不重复使用旧 token。
- 释放确认匹配完整节点、实例和房间代次；迟到释放不能清掉新绑定。退役记录覆盖尚未使用的短期 JWT，
  等控制面确认且凭证保护期结束后删除；未确认记录有数量上限，满时拒绝新增以防无界增长。
- 结束会议先持久化 `closing`，立即禁止准入；owner 清理后置 `ended`，释放节点和房主未结束会议额度。
  HTTP 失败时保留 `closing`，可重试，也可由后续节点心跳继续清理。
- 既有绑定不会因节点失联而自动迁移；控制进程重启保留会议、成员、绑定及退役实例。
  旧数据库自动补列并生成绑定代次；升级前不带代次的节点 JWT 需要重新申请。

### 内部连接配置与失败语义

SFU 配置增加 `management_url`、`internal_listen_ip`、`internal_http_port`。
本机模板使用 `http://127.0.0.1:19080` / `19081`，由控制面通过该地址发送命令；节点通过
`PACKETIA_NODE_SECRET` 认证，控制进程在 `PACKETIA_NODE_SECRETS` 中配置对应密钥。
省略管理 URL 仍可兼容原准入流程，但后台远程管理不可用。已有本地启动脚本为新增节点生成独立内部端口并检查冲突：

```sh
cmake --build build/webrtc-integration --target PacketiaControl PacketiaSfu -j6
python3 scripts/run_cluster_local.py --build-dir build/webrtc-integration --nodes 2
```

owner 管理通道每个最多 64 条、4 MiB；单次等待最多 1 秒。HTTP 连接建立最多 500 ms，
节点命令总请求最多 2 秒，响应最多 512 KiB；满队列、不可达或超时均返回错误。
尚未开始且已取消的命令不会迟到执行；已经开始执行的操作可能在 HTTP 超时前后完成，
当前没有通用命令去重和持久结果查询，不能把超时当作一定未执行，应查询当前状态后决定是否重试。
控制进程停止顺序见上面的统一生命周期说明；SFU 仍先排空内部 HTTP worker，再结束心跳与 owner。

相关验证入口：

```sh
ctest --test-dir build/webrtc-integration --output-on-failure \
  -R '^(ServiceCoreUnitTest|MeetingModuleUnitTest|ClusterUnitTest|ClusterApiUnitTest|MeetingControlIntegrationTest|webrtc_control)$'
```

测试包含生命周期幂等、初始化失败回滚、停止期间拒绝新操作并排空已接收操作、重新初始化、
旧 HTTP 路由的停止检查；以及假时钟的预留超时、旧库迁移、代次释放和额度恢复；真实 WebSocket 的后端撤权、
最后一人踢出、媒体资源回收和排队命令超时；一控两 SFU 的管理转发、恢复权限、结束与控制面重启。
未覆盖公网部署、容量压测、节点故障迁移和跨进程媒体恢复。

## 房间鉴权

`service/Auth` 按 LiveKit 的职责划分实现：API key 查询签名密钥、JWT 验证可信声明、
房间策略检查操作权限、WebRTC 成员保存已验证身份与权限。运行时使用 OpenSSL 的 HS256，
无需额外 JWT 库；主进程通过配置创建鉴权对象，信令回调中不访问数据库或远程服务。

```text
业务后端确认登录与房间权限 → 签发房间 JWT → 浏览器 join/resume
  → iss 查询 API secret → 验证签名/用途/时间 → 检查 roomJoin 与房间
  → 绑定 Participant.subject_id 与权限 → publish/subscribe 按绑定权限授权
```

JWT 使用 `iss=API key`、`sub=用户身份`、`aud=packetia-webrtc`、`exp`，以及
`video: {room, roomJoin, canPublish, canSubscribe, roomAdmin, canUpdateOwnMetadata}`。仅显式为 `true` 的权限允许执行；
浏览器不能自报身份或通过更换后续消息里的 token 提升权限。旧的独立 `offer` 入口已关闭，
须先 `join` 再 `publish/subscribe`；现有房间协商只支持专门的 `restart_ice` 恢复流程。

`join`、`resume` 验证 JWT 有效期；已建立连接持续使用绑定权限，token 到期不会自动断流。
恢复还需同一签发者、用户、房间及房间绑定代次，以及服务端生成的 `resume_token`；超过恢复窗口则重新入会。
原 JWT 到期时先向业务签发端获取同身份、同房间的新 JWT；本版不在 SFU 自动刷新 token。
恢复使用服务端保存的会内有效权限，新 JWT 中的发布、订阅、管理及资料权限均不覆盖现有成员。
暂不提供 JWT 撤销列表。TURN 长期凭据和 ICE 临时凭据各自独立。

服务端配置 `webrtc.api_keys` 或成对设置环境变量；secret 至少 32 字节，永远不发给浏览器。
旧 `webrtc.token` / `PACKETIA_WEBRTC_TOKEN` 会返回迁移错误。

```sh
export PACKETIA_WEBRTC_API_KEY=local-dev
export PACKETIA_WEBRTC_API_SECRET="$(openssl rand -hex 32)"
python3 tools/create_room_token.py --identity alice --room demo --can-publish
python3 tools/create_room_token.py --identity bob --room demo --can-subscribe
```

两次输出分别是发布者和观众的 JWT。签发工具在可信业务后端运行；线上由登录/入会接口
决定 identity、room 和权限，再通过 HTTPS 返回 token；信令经反向代理配置 WSS，
当前本地示例仍为 WS，不代表已接入原生 WSS 监听。具体声明、配置、信令格式和限制见
[鉴权模块说明](#房间鉴权)。

### 会议成员管理

以下 WebSocket 接口已接入 `RoomSignalHandler`，统一在房间 owner 上处理。`join`、`resume`
回复保留原字段，增加自己的 `participant` 快照及成员 `revision`；查询包含断线恢复中的成员。

| 请求 `type` | 参数 | 返回 `type` / 授权 |
| --- | --- | --- |
| `join` | `room_id`, `token`；可选 `name`, `metadata`, `attributes` | `joined`；附带资料时需自改资料权限 |
| `leave` | 无 | `closed`；清理成员、媒体及恢复凭据，连接仍可重新入会 |
| `participants` | 可选 `limit`（默认 20，1..100）、`cursor`, `revision` | `participants`；房间内成员均可查询 |
| `participant` | `participant_id` | `participant`；只查询当前房间 |
| `update_participant` | 可选 `participant_id`（默认自己），以及资料或 `permissions` 补丁 | `participant_updated`；修改自己需 `canUpdateOwnMetadata` 或 `roomAdmin`，修改他人或任何权限需 `roomAdmin` |
| `remove_participant` / `kick` | `participant_id` | `participant_removed`；需 `roomAdmin`，移除自己请用 `leave` |

成员快照包含 `participant_id`、`subject_id`、`name`、`state`、`metadata`、`attributes` 和 `permissions`，
不包含连接标识、JWT 或恢复凭据。资料及权限请求全部校验后才提交，省略字段保留原值。
姓名最多 256 字节，元数据是最多 4096 字节的字符串；属性按键合并，`null` 删除键，空字符串保留。
最终属性最多 32 项，键 1..64 字节、值最多 512 字节，键和值合计最多 8192 字节。

```json
{"type":"update_participant","name":"Alice","metadata":"{\"department\":\"media\"}","attributes":{"team":"audio","old_key":null}}
```

```json
{"type":"update_participant","participant_id":"p-...","permissions":{"can_publish":false,"can_subscribe":true,"can_update_own_metadata":true,"room_admin":false}}
```

权限撤销立即检查现有媒体角色，停止失去授权的发布或订阅会话，释放端点、转发关系及 UDP 路由；
成员仍留在房间。被修改的在线成员收到 `permissions_changed`，其中 `media_stopped` 指示是否需关闭本地媒体连接。
重连不会借旧 JWT 恢复已撤销权限。踢人同时清理媒体和恢复凭据，在线客户端收到 `removed` 后限时关闭连接，
应停止自动恢复。踢人是移除本次入会，不是永久封禁；有效入房 JWT 仍可发起全新入会。

`Room` 将入会、离会、资料及恢复状态变化发布为 `ParticipantChangedEvent`。
信令订阅 Bus 后仅维护最新版本槽，在 Tick 中合并发送 `participants_changed {room_id, revision}`；
客户端收到后重新查询。通知使用现有有界响应通道，首条响应先于通知，慢客户端不会无限堆积成员快照。
成员分页还按 JSON 编码字节数限制页面大小，实际返回人数可能小于 `limit`。
后续页必须带上一页的 `next_cursor` 和同一个 `revision`；`next_cursor` 为空表示结束。
期间发生变更会返回 `code=snapshot_changed`，客户端应丢弃已收集页面并重新查询。
版本属于当前房间实例，重新入会后应重新获取成员快照。

控制服务签发的房间创建者 JWT 带 `roomAdmin=true`，普通成员不带管理权限；
控制入口和本机开发示例均显式允许修改自己的资料。单进程部署可在可信后端签发：

```sh
python3 tools/create_room_token.py --identity owner --room demo --room-admin --can-publish --can-subscribe --can-update-own-metadata
```

浏览器示例已分流新通知，并通过 `window.demo.listParticipants()`、`getParticipant(id)`、
`updateParticipant(patch, id)`、`removeParticipant(id)` 提供调用入口；示例签发器不会授予房间管理权限。
这些更新目前只作用于 SFU 进程内的本次成员生命周期，不持久化到账号、控制服务成员表或下一次入会凭证。

相关回归：`webrtc_members`（真实 WebSocket、媒体资源及 Bus）、`AuthFrameworkUnitTest`、
`ClusterUnitTest` 和 `ClusterApiUnitTest`。浏览器页面操作、公网并发与容量仍需单独验证。

## RTP 协议规范

本节整理项目涉及的主要 RTP 协议规范，便于读代码和继续实现。字段和处理要求以
RFC 正文及勘误为准；规范要求与当前支持范围分别说明，不代表项目已完整实现所有 RFC。
RTP/RTCP 负责媒体封装、来源、排序、时间与反馈，SDP 负责协商格式，ICE/TURN 负责连通与中继。

### 主要 RFC

| 功能 | 规范 | 重点 |
| --- | --- | --- |
| RTP/RTCP 基础 | [RFC 3550](https://www.rfc-editor.org/rfc/rfc3550) | 固定头、报告、同步、SSRC 管理、接收统计 |
| 音视频 profile | [RFC 3551](https://www.rfc-editor.org/rfc/rfc3551) | 静态/动态 PT、时钟、marker |
| 通用扩展头 | [RFC 8285](https://www.rfc-editor.org/rfc/rfc8285) | 单字节/双字节格式、extmap、混合格式；取代 RFC 5285 |
| SDP 与 Offer/Answer | [RFC 8866](https://www.rfc-editor.org/rfc/rfc8866)、[RFC 3264](https://www.rfc-editor.org/rfc/rfc3264) | rtpmap、fmtp、方向和能力交集 |
| RTP/RTCP 同端口 | [RFC 5761](https://www.rfc-editor.org/rfc/rfc5761) | rtcp-mux、PT 冲突限制 |
| BUNDLE/MID | [RFC 8843](https://www.rfc-editor.org/rfc/rfc8843) | 多媒体段共用传输及分流 |
| RID/simulcast | [RFC 8852](https://www.rfc-editor.org/rfc/rfc8852)、[RFC 8853](https://www.rfc-editor.org/rfc/rfc8853) | 编码流标识与多编码协商 |
| 反馈 | [RFC 4585](https://www.rfc-editor.org/rfc/rfc4585)、[RFC 5104](https://www.rfc-editor.org/rfc/rfc5104) | AVPF、NACK、PLI、FIR |
| RTX | [RFC 4588](https://www.rfc-editor.org/rfc/rfc4588) | OSN、apt、独立重传序列号空间 |
| Reduced-size RTCP | [RFC 5506](https://www.rfc-editor.org/rfc/rfc5506) | 独立反馈包与周期性 compound RTCP |
| 扩展报告/CNAME | [RFC 3611](https://www.rfc-editor.org/rfc/rfc3611)、[RFC 7022](https://www.rfc-editor.org/rfc/rfc7022) | XR、跨流来源关联 |
| 拥塞反馈与停止条件 | [RFC 8888](https://www.rfc-editor.org/rfc/rfc8888)、[RFC 8083](https://www.rfc-editor.org/rfc/rfc8083) | 标准拥塞反馈、circuit breaker |
| SRTP/DTLS-SRTP | [RFC 3711](https://www.rfc-editor.org/rfc/rfc3711)、[RFC 7714](https://www.rfc-editor.org/rfc/rfc7714)、[RFC 5764](https://www.rfc-editor.org/rfc/rfc5764) | 认证、加密、重放防护与密钥协商 |
| WebRTC 分流与媒体要求 | [RFC 7983](https://www.rfc-editor.org/rfc/rfc7983)、[RFC 8834](https://www.rfc-editor.org/rfc/rfc8834) | STUN/DTLS/RTP/RTCP 分流 |
| 冗余/FEC | [RFC 2198](https://www.rfc-editor.org/rfc/rfc2198)、[RFC 5109](https://www.rfc-editor.org/rfc/rfc5109)、[RFC 8627](https://www.rfc-editor.org/rfc/rfc8627) | RED、ULPFEC、FlexFEC |

### RTP 基础头：RFC 3550 §5.1

```text
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|V=2|P|X|  CC   |M|     PT      |       sequence number         |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                           timestamp                           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|           synchronization source (SSRC) identifier            |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|               CSRC identifiers (0..15 entries)                 |
|                              ...                              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|       extension envelope and data, present only if X=1        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                   payload and optional padding                |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

多字节整数采用网络字节序，即大端；固定头 12 字节，CSRC、扩展、载荷及尾部填充不在该长度内。
`#pragma pack` 只控制内存布局，不能代替字节序转换，也不能保证非对齐访问安全。

| 字段 | 位数 | 含义与约束 |
| --- | --- | --- |
| V | 2 | RTP 版本为 2，与 SDP 的 v=0 无关 |
| P | 1 | 存在尾部填充；最后一字节表示填充总长度，包括自身，不得为 0 |
| X | 1 | CSRC 后有一个扩展封套，封套内可有多个元素 |
| CC | 4 | CSRC 个数，0..15，每项 4 字节 |
| M | 1 | 含义由 profile/载荷规范定义，并非统一关键帧标志 |
| PT | 7 | 载荷格式映射，不是全局轨道标识 |
| sequence | 16 | 每个原始包加 1，按 65536 回绕，初始值应随机 |
| timestamp | 32 | 采样时刻，按载荷时钟计数及 2^32 回绕，初始值应随机 |
| SSRC | 32 | 会话内同步源标识，随机选择并处理碰撞 |
| CSRC | 每项 32 | 混流器输出的贡献源列表，不能当作 MID |

- 常用静态 PT：PCMU=0、PCMA=8；96..127 为常用动态区间。H264=96、Opus=111 是本地能力示例，实际收发使用 SDP 协商值。
- rtcp-mux 模式下不能使用 PT 64..95，以免和 RTCP 类型冲突；分流后仍需校验长度与结构。
- 同一视频帧的多个包通常时间戳相同、序列号不同。序列号比较需处理回绕、乱序、重复和丢包，不能直接比较普通整数大小。
- H264/H265 通常采用 90000 Hz 时钟；30 fps 相邻采样帧通常相差 3000。Opus RTP 时钟固定为 48000 Hz，20 ms 相差 960。
- 时间戳按采样时间产生，不是收包时间；传输顺序不同时可能不单调。音频 DTX 不发包时，媒体时钟仍推进。
- SSRC、MID、RID、PT 分别标识来源、媒体段、编码流、载荷格式。不同流的时间戳不能直接比较，需通过 RTCP SR 的 NTP/RTP 对和 CNAME 建立同步关系。

### 扩展封套与元素：RFC 3550 §5.3.1 / RFC 8285

```text
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|       defined by profile      |       length (32-bit words)   |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                 extension data and alignment padding          |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

One-byte, profile=0xBEDE:
+-+-+-+-+-+-+-+-+-------------------------------+
|  ID   |  len  |       data (len + 1 bytes)      |
+-+-+-+-+-+-+-+-+-------------------------------+

Two-byte, profile=0x1000 | appbits:
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-------------------------------+
|       ID      |     length    |       data (length bytes)      |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-------------------------------+
```

封套在 `12 + 4 * CC` 字节处。length 是扩展数据的 32 位字数，不含封套自身 4 字节，允许为 0。
未知 profile 按封套长度跳过；元素逐字节解析，整个扩展数据区用零字节补至 4 字节边界。

- 单字节 ID=1..14，len=0..15，实际数据 1..16 字节；ID=15 时忽略 len 并停止元素解析。
- RFC 8285 §4.1.2 还要求遇到 ID=0 且 len 非零时停止解析。
- 双字节 ID=1..255，包括 ID=15；length 直接表示 0..255 字节，不加 1。profile 低 4 位 appbits 未约定时发送端应置零，接收端忽略。
- 两种格式中的 0x00 都只占一个 padding 字节；双字节格式不再读取其后的 length。
- 同一包只采用一种元素格式。流内混用两种格式需通过 extmap-allow-mixed 或带外方式确认接收方支持。
- 扩展 ID 是 SDP extmap 协商的局部编号，URI 才标识含义；遵守收发方向及 BUNDLE 映射一致性。下游转发按订阅者的编号重建扩展。
- 自定义“私有头”应约定扩展 URI 和数据编码，不改变固定头或占用保留 ID，也不直接发送 C++ 结构体内存。

```sdp
a=extmap:1 urn:ietf:params:rtp-hdrext:sdes:mid
a=extmap:3 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01
```

上例的 1 和 3 不是标准固定编号。常见扩展还包括 RID/repaired RID，以及
[RFC 6464](https://www.rfc-editor.org/rfc/rfc6464) 的音量扩展。
当前 TWCC 使用上例草案格式，与 RFC 8888 的拥塞反馈不是同一种线格式。

```text
base_header_bytes = 12 + 4 * CC
extension_bytes   = X ? 4 + 4 * extension_length_words : 0
payload_offset    = base_header_bytes + extension_bytes
padding_bytes     = P ? packet[last] : 0
payload_bytes     = packet_bytes - payload_offset - padding_bytes
```

每一步必须先校验长度。尾部填充不能侵入头部/扩展。RTP 尾部填充、扩展对齐填充、SRTP
认证标签/MKI 是不同概念；上述公式仅用于去除 SRTP 尾部保护数据后的明文 RTP。

### 常见媒体载荷

载荷头位于 RTP 头之后，与 RTP 扩展头分开解析。

| 编码 | 规范 | 主要内容 |
| --- | --- | --- |
| H264 | [RFC 6184](https://www.rfc-editor.org/rfc/rfc6184) | Single NAL、STAP-A、FU-A；packetization-mode/profile-level-id 协商 |
| H265 | [RFC 7798](https://www.rfc-editor.org/rfc/rfc7798) | 单 NAL、AP、FU、PACI；DONL/DOND 取决于协商 |
| Opus | [RFC 7587](https://www.rfc-editor.org/rfc/rfc7587) | 一个 RTP 载荷携带一个 Opus packet；TOC/帧结构另见 RFC 6716 |
| PCMU/PCMA | RFC 3551 | 常见单声道 8 kHz；marker 可表示 talkspurt 开始 |
| AAC MPEG4-GENERIC | [RFC 3640](https://www.rfc-editor.org/rfc/rfc3640) | AU-headers-length 以位计；字段长度由 fmtp 约定 |
| AAC MP4A-LATM | [RFC 6416](https://www.rfc-editor.org/rfc/rfc6416) | LATM 格式，不能按 MPEG4-GENERIC 解析 |
| VP8 | [RFC 7741](https://www.rfc-editor.org/rfc/rfc7741) | 载荷描述符、分区、PictureID 与时间层 |

```text
Single NAL: [F:1 | NRI:2 | Type:5] [NAL data ...]     Type=1..23
STAP-A:     [F:1 | NRI:2 | Type=24]
            [NAL size:16] [complete NAL ...]          repeated
FU-A:       [F:1 | NRI:2 | Type=28]
            [S:1 | E:1 | R:1 | original Type:5] [fragment ...]
```

FU-A 的 S/E 表示 NAL 分片开始/结束，不能同时为 1，R 为保留位。重组恢复原 NAL 头并校验
序列连续性，不能把缺少分片的 NAL 输出为完整数据。H264 的 M 表示 access unit 最后一个包，
FU 的 E 表示某个 NAL 的结束，两者不同。RTP 载荷不包含 Annex-B 起始码，由解包输出时添加。

### RTCP、同步和接收质量

```text
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|V=2|P| count/FMT | packet type   |           length              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    packet-specific body                       |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

RTCP length 表示整个单包的 32 位字数减 1，包含公共头，字节数为 `(length + 1) * 4`；
与 RTP 扩展的 length 定义不同。数据报可能包含多个 RTCP 单包，要逐个校验。

| PT | 类型 | 内容 |
| --- | --- | --- |
| 200 | SR | SSRC、NTP/RTP 时间对、累计发包/载荷字节数和报告块 |
| 201 | RR | 报告者 SSRC 和接收报告块 |
| 202 | SDES | CNAME 等来源描述 |
| 203 | BYE | 来源离开及可选原因 |
| 204 | APP | 应用自定义内容 |
| 205 | RTPFB | 如 FMT=1 的 Generic NACK |
| 206 | PSFB | 如 FMT=1 PLI、FMT=4 FIR |
| 207 | XR | 扩展报告 |

每个接收报告块 24 字节：SSRC(32)、fraction lost(8)、cumulative lost(24 signed)、
extended highest seq(32)、jitter(32)、LSR(32)、DLSR(32)。累计丢包是有符号 24 位值。
LSR 为最近 SR 的 NTP 中间 32 位，DLSR 单位为 1/65536 秒。

```text
transit_i = arrival_i_in_RTP_clock_units - RTP_timestamp_i
D         = transit_i - transit_(i-1)
J         = J + (abs(D) - J) / 16
RTT       = A - LSR - DLSR
```

J 的单位为 RTP clock tick，不直接是毫秒。A 是当前 NTP 中间 32 位；RTT 计算需处理回绕，
没有可用 LSR 时不能使用此公式。不同 SSRC 的时钟和统计不能混算。
基础 compound RTCP 通常以 SR/RR 开始并包含 SDES CNAME。发送间隔按成员数、带宽、随机化
及 reconsideration 计算；AVPF 提前反馈、RFC 5506 独立反馈包也有调度及周期性报告要求。

### 重传、冗余与 SFU 转发

Generic NACK 的每项 FCI 为 PID(16)+BLP(16)：PID 是第一个丢失序列号，BLP 第 i 位表示
`(PID + i + 1) mod 65536` 也丢失。实现需处理回绕、重复反馈、缓存过期和重传限频。
PLI 没有 FCI，表示图片丢失，不列出具体包，也不保证立即收到 IDR。
FIR 的 FCI 包含目标 SSRC 和请求序号，不能按 PLI 解析；发布者/编码器需实际响应请求。

RTX 通过 apt 关联原 PT，采用独立重传序列号，载荷开头为 16 位 OSN；SSRC 复用模式
使用独立 RTX SSRC 并通过 FID 分组关联。直接重发原包与 RFC 4588 RTX 封装是不同方案。
原包重发保留 RTP sequence，但每次网络发送分配新的 TWCC sequence；同时考虑 SRTP 重放窗口。
RED、ULPFEC、FlexFEC 分别协商并实现恢复；Opus useinbandfec 不代表通用 RTP FEC 支持。

SFU 应先分流并认证/解密 SRTP，再解析 RTP；不能让未认证的扩展修改轨道绑定。
BUNDLE 内按协商 MID 和绑定 SSRC 分流，下游按订阅者协商值改写 PT、MID、SSRC 和传输序号，
相关 RTCP 也需正确映射。接收/发送统计按来源维护，TWCC 使用传输级序列号空间。
MTU 扣除 IP/UDP、变长 RTP 头、SRTP 和 TURN 开销；固定 payload 上限不是 RFC 通用要求。

### 当前实现范围

| 入口 | 当前能力与边界 |
| --- | --- |
| [Rtp.h](Rtsp/Rtp.h)、[Rtp.cpp](Rtsp/Rtp.cpp) | 固定头与接收统计；完整 SSRC 碰撞管理和 RTCP 调度仍需核对 |
| [RtpReceiver.cpp](Rtsp/RtpReceiver.cpp) | 版本/PT/SSRC 校验，按 CC/X/P 计算载荷位置 |
| [RtpHeaderExtensions.h](Rtsp/Rtp/RtpHeaderExtensions.h) | 单/双字节解析，未知 profile 跳过；重复 ID 按本地策略拒绝；ID=0 且 len 非零的停止规则待补，失败可能留下部分输出 |
| [RtpTransportCcExtension.cpp](Rtsp/RtpTransportCcExtension.cpp) | 仅单字节 TWCC、ID 1..14、两字节序号 |
| [RtpSenderTrack.cpp](Rtsp/RtpSenderTrack.cpp) | 下游改写、缓存、NACK 原包重发；MID/TWCC 输出为单字节，尚非完整 RTX |
| [H264RtpPayloadParser.cpp](Common/Depacketizer/H264RtpPayloadParser.cpp) | Single NAL/STAP-A/FU-A，不代表全部 H264 打包模式 |
| [AudioRtpDepacketizerFactory.cpp](Common/Depacketizer/AudioRtpDepacketizerFactory.cpp) | G.711/Opus 接收工厂，其他路径/编码需单独核对 |
| [RtcpReciver.cpp](protocol/Rtcp/RtcpReciver.cpp) | SR/RR/BYE、NACK/PLI/FIR、传输反馈入口；当前跳过 SDES，枚举不代表处理器已实现 |
| [SdpNegotiator.cpp](src/sdp/SdpNegotiator.cpp) | PT/codec、方向、BUNDLE、扩展协商；WebRTC 使用 UDP DTLS-SRTP 与 rtcp-mux |

后续按扩展边界处理、SDES/CNAME 与 compound RTCP、RTX、RID/simulcast、拥塞反馈、FEC
逐项推进，验证协商、实际收发与退出清理。

## TURN UDP 中继

`protocol/turn` 提供 TURN 模块，由 `ServerApp` 在 `Packetia` 主进程内统一启动、关闭，
与 WebRTC 直接组合，不需要 RPC 或另外启动 TURN 进程。支持 IPv4/IPv6 UDP 控制与中继套接字。
支持长期凭证认证、401/438 challenge、Allocate/Refresh、CreatePermission、
Send/Data indication、ChannelBind/ChannelData、生命周期清理、配额与 peer ACL。
同一个 UDP 控制端口支持基础 STUN Binding 地址发现。当前不包含 TCP/TLS，不能等同于 coturn 全功能。
默认关闭，在配置文件中设置 `turn.enabled=true`，或通过 `PACKETIA_TURN=true` 开启。
账号由 `PACKETIA_TURN_USER` 和 `PACKETIA_TURN_PASSWORD` 环境变量提供，要求非空可打印 ASCII。
TURN 开启后，缺少账号、绑定失败或后续模块启动失败都会触发启动回滚。
网络参数与配额放在 JSON 的 `turn` 节，字段见 `config/packetia.example.json` 和 `config/AppConfig.h`。

```sh
cmake --build build/webrtc-integration --target Packetia
export PACKETIA_CONFIG="$PWD/config/packetia.example.json"
export PACKETIA_TURN=true
export PACKETIA_TURN_USER=packetia-test
export PACKETIA_TURN_PASSWORD=local-test-password
export PACKETIA_WEBRTC=true
export PACKETIA_WEBRTC_PUBLIC_IP=127.0.0.1
export PACKETIA_WEBRTC_API_KEY=local-dev
export PACKETIA_WEBRTC_API_SECRET="$(openssl rand -hex 32)"
build/webrtc-integration/Packetia
```

公网运行需配置监听/中继/公告 IP 与 relay 端口范围，并放通对应 UDP 端口。
默认 peer ACL 拒绝私网、回环、链路本地及组播目标；配置 `turn.local_test=true` 仅允许回环 peer。
控制监听与 SFU 媒体仍使用不同的 UDP 端口，浏览器仍需在 `iceServers` 中配置 TURN URL 和账号。
启动先完成 I/O 和工作线程，再启动 TURN 与网络模块；关闭时先停止 WebRTC，最后停止 TURN 和 I/O。
`PacketiaTurn` 入口保留作独立协议调试工具，正常运行使用上面的 `Packetia` 入口即可。

### UDP 401 鉴权响应限流

参考 Coturn 的 `401-ratelimit` 设计，增加按来源 IP 限制 UDP 401 响应的能力，降低伪造源地址
引发的反射放大流量。默认关闭，在配置文件中开启：

```json
{
  "turn": {
    "unauthorized_ratelimit": true,
    "unauthorized_ratelimit_rps": 10
  }
}
```

`unauthorized_ratelimit_rps` 必须为正的 32 位整数，非法值拒绝加载，不自动回退。
每个桶从首次使用开始保留 1000 毫秒窗口，允许前 N 次 401 响应，超出后静默丢弃响应，
不再发送额外错误。新请求与缓存重传都计入预算；来源端口不参与计算，IPv4-mapped IPv6
统一为 IPv4。同一 NAT 公网 IP 下的客户端共享预算，应按实际客户端数量设置阈值。

`TurnAuthRateLimiter` 使用固定 4096 个桶，不动态增长；不同 IP 落入同一有效桶时共享预算，
不会覆盖已有计数获取新额度，因此可能误限正常来源。计数饱和后不再递增，防止溢出绕过；
超限与碰撞日志分别每桶每窗口最多记录一次。
当前 TURN 对象由单个调度线程串行处理，限流表也归该线程所有，无需跨线程原子计数。
每个 `TurnServer` 实例独立计数，停止时清空窗口；主程序目前只创建一个实例，不提供集群共享额度。

`TurnServer::GetStats()` 提供 `unauthorized_401_requests`、`unauthorized_401_responses`、
`unauthorized_401_dropped`、`ratelimit_hash_collisions`、`ratelimit_occupied_buckets` 和
`ratelimit_total_buckets`。响应计数表示 UDP socket 接受发送，不保证对端收到；发送失败
不会计作限流丢弃。桶占用数在获取快照时扫描，不在请求路径扫描。尚未提供 Prometheus HTTP 导出。

这一功能只控制 UDP 401 响应，不改变现有长期凭证鉴权，不限制已鉴权请求或媒体转发，
也不限制 STUN Binding、438 等其他响应。当前 nonce 仍按会话维护，静默丢弃不会消除
会话及响应缓存的资源消耗；stateless nonce、未验证来源的其他错误预算需另行实现。

验证入口：

```sh
cmake --build build/webrtc-integration --target turn_unit_tests test_server_config test_server_app --parallel 4
ctest --test-dir build/webrtc-integration --output-on-failure \
  -R '^(TurnUnitTest|server_config|server_app)$'
```

### 同端口 STUN 地址发现

TURN 开启后，浏览器可在同一地址使用 `stun:主机:端口` 查询自身映射地址，
也可使用 `turn:主机:端口?transport=udp` 加账号申请中继。Binding 不需要 TURN 凭据，
返回服务器实际看到的来源 IP/端口（`XOR-MAPPED-ADDRESS`），保留请求事务 ID，附带 FINGERPRINT。
它不返回中继地址，不创建会话或 Allocation，不占用 TURN 会话配额，也不会刷新已有资源的有效期。

IPv4/IPv6 客户端均支持，双栈监听下的 IPv4-mapped 地址返回为 IPv4。
请求允许没有 FINGERPRINT；携带时必须校验成功。未知必需属性返回 420 并列出属性类型，
未知可选属性及已识别但本用途不使用的属性被忽略。该入口不执行 MESSAGE-INTEGRITY 身份认证，
响应不携带 MESSAGE-INTEGRITY；ICE 连通性检查的认证仍由 SFU 的 IceAgent 处理。
重传按当前来源地址重新计算响应，不建立事务缓存。畸形报文、非请求消息以及超过 4096 字节的请求被丢弃。
目前仅提供现代 STUN 的 UDP Binding 子集，不包含 TCP、RFC 3489 兼容或 RFC 5780 NAT 行为探测。

### TURN 双栈与 IPv6 中继

默认仍使用 IPv4。在 JSON 中设置 `turn.dual_stack=true`、`turn.listen_ip="::"` 后，一个 IPv6 控制 socket
同时接收 IPv4/IPv6 客户端；接收的 IPv4-mapped 地址统一为 IPv4，回复时由 socket 层转回映射形式。
不启用双栈的 IPv6 监听设置 `IPV6_V6ONLY=1`。

`turn.relay_bind_ip` / `turn.advertised_ip` 配置 IPv4 中继绑定/公告地址，
`turn.relay_bind_ip_v6` / `turn.advertised_ip_v6` 配置 IPv6 中继绑定/公告地址。两组配置独立，
IPv6 中继默认关闭；程序接口中清空某组的两个地址可关闭该地址族。
客户端未携带 `REQUESTED-ADDRESS-FAMILY` 时仍请求 IPv4，值 `0x02` 请求 IPv6。
未配置所请求的地址族返回 440，对端地址族与中继不匹配返回 443。
IPv4 客户端可申请 IPv6 中继，IPv6 客户端也可申请 IPv4 中继。
每个 Allocation 当前只分配一种地址族，不支持 `ADDITIONAL-ADDRESS-FAMILY` 双重分配。

本机双栈测试配置，可合并到配置文件并使用上述主程序入口：

```json
{
  "turn": {
    "enabled": true,
    "listen_ip": "::",
    "listen_port": 3478,
    "dual_stack": true,
    "relay_bind_ip": "127.0.0.1",
    "advertised_ip": "127.0.0.1",
    "relay_bind_ip_v6": "::1",
    "advertised_ip_v6": "::1",
    "local_test": true
  }
}
```

公网部署应改为实际接口及可达的公告地址，并放通 IPv4/IPv6 的控制端口和中继端口范围。
IPv6 默认 ACL 同样拒绝回环、ULA、链路本地、组播及映射/兼容 IPv4 地址。
当前仅接受无 zone ID 的数字 IP；不支持携带 scope 的链路本地对端。
TURN 测试覆盖四种接入/中继地址族组合的真实 UDP 转发、通道、权限到期和资源释放。
SFU 的双栈监听与双地址 SDP 候选发布尚未在这一功能中实现。

## 媒体层结构与命名

媒体层以发布源和转发关系为中心，分开保存源描述、发布元数据和消费关系。
目录统一使用小写，生产文件使用 PascalCase，测试使用 `tests/test_snake_case.cpp`；
类与头文件名称对应，拆分实现时增加职责后缀。CMake 显式维护质量模块源列表，避免新增文件被隐式编入目标。

| 目录 | 主要职责 |
| --- | --- |
| `media/types/` | 媒体类型、编码身份及进程内 `MediaTrackRef` |
| `media/track/` | 通用源描述、发布信息值快照及线程安全存储 |
| `media/core/` | 流生命周期、全局目录、受管理消费句柄和事件总线 |
| `media/frame/` | 帧类型、编码帧发布接口与路由；包含 `EncodedFrameRouterBinding.cpp` |
| `media/endpoint/`、`media/transport/` | 现有协议相关收发运行时与传输适配 |
| `media/factory/` | 现有 RTP 发送轨道工厂 |
| `media/codec/`、`media/quality/`、`media/stats/` | 编解码、质量控制和统计；BWE 文件统一为 PascalCase |
| `media/tests/`、`media/quality/tests/` | 核心与质量模块测试 |
| `Rtsp/Rtp/` | RTSP/WebRTC 复用的 RTP 参数与 SDP 适配 |
| `Room/` | 房间成员、权限、发布订阅关系，以及房间业务事件 |

| 类型 | 原名称 | 含义 |
| --- | --- | --- |
| `MediaTrackDescription` | `MediaStreamTrack` | 协议无关的源描述：轨道 ID、类型、编码、音频采样率/声道与源引用 |
| `TrackPublicationInfo` | `TrackMetadata` | 发布信息值快照，包含业务 ID、名称、静音等元数据 |
| `TrackPublication` | `MediaTrack` | 加锁存储和更新发布信息，不表示媒体接收器已经就绪 |
| `TrackPublicationPtr` | `MediaTrackPtr` | 发布信息对象的共享引用 |
| `MediaTrackSubscription` | `MediaTrackConsumer` | 某个消费者到源轨道的关系句柄及投递闸门 |

`TrackPublicationInfo` 为兼容现有信令保留 MID、CID、编码层等字段，不能用作内核注册的通用源描述。
`MediaStreamRegistry` 的 `AttachConsumer`、`SetConsumerPaused`、`DetachConsumer` 仍描述消费者操作，
句柄使用 `MediaTrackSubscription`。房间业务订阅与内核消费关系分别管理权限意图和实际投递，二者不互相替代。

`packetia_media_core` 编译 `core/` 与 `frame/`，依赖基础类型、共享内存、线程库和日志库；
`media` 是包含协议适配的聚合目标。独立录制、混流和转码构建可直接编译
`frame/EncodedFrameRouter.cpp`；需要注册表的订阅重载单独放在 `EncodedFrameRouterBinding.cpp`，
不把全局目录依赖带入基础帧路由实现。

### SFU 端点职责

`SfuEndpoint` 使用组合管理下行发送、帧分发和接收质量，保留原有公开入口供房间和协议层调用。

| 组件 | 拥有的状态与职责 |
| --- | --- |
| `SfuEndpoint` | 发布轨道、MID/SSRC 绑定、接收器、RTP/RTCP 入口、跨端点关系登记与运行状态 |
| `DownlinkTrackManager` | 下行协商参数、订阅表、RTP 发送器、消费出口闸门、RTCP 发送器注册及退订清理 |
| `frame/EncodedFrameDispatcher` | 来源描述、帧回调快照、公共发布器和发布计数；不依赖端点类型 |
| `quality/ReceiveQualityMonitor` | 按接收 SSRC 管理独立控制器，输入统计数据，输出质量及控制建议；不访问接收器或网络 |
| `endpoint/ReceiveFeedbackHandler` | 推进接收器 NACK、采集统计、生成并发送 RR，将统计交给质量模块，返回关键帧请求建议 |

订阅链路为 `Room::SubscribeTrack` → `SfuEndpoint::Subscribe` → `DownlinkTrackManager::Subscribe`，
端点统一取得源端和目标端的状态锁，管理器创建发送器，成功后在源轨道登记同一关系的弱引用。
完整帧则从接收器回到 `frame/EncodedFrameDispatcher`，再进入 `EncodedFrameRouter` 和帧回调。
接收反馈链路为 `SfuEndpoint` → `endpoint/ReceiveFeedbackHandler` → `quality/ReceiveQualityMonitor`
→ `WeakNetController`；反馈协调器提取视频关键帧请求，由端点执行。RR 仍通过原有 RTCP 出口发送。
WebRTC 在创建端点时通过构造参数传入来源描述，信令层不再通过派生类修改端点内部来源字段。

组件复用现有媒体 Worker。下行管理器的可变状态由端点的 `runtime_mutex_` 保护，
RTCP 分发器比下行管理器存活更久；清理先关闭关系闸门，再注销 RTCP 索引，最后释放发送器引用。
已排队任务即使仍持有旧关系，也会在投递前检查关闭状态。
帧回调和被移除回调的捕获对象在帧分发器锁外调用或释放；接收质量状态使用组件内部锁。
`SfuEndpointTypes.h` 只保存端点发布/订阅关系；帧回调类型由帧分发器声明，质量控制状态由质量模块持有，
删除没有读取方的重复 RR 与质量快照缓存。发布协商、接收器生命周期与恢复调度仍由 `SfuEndpoint` 协调，
接收侧 NACK 推进和 RR 发送由反馈协调器执行；组件拆分不改变房间 owner 和网络执行器归属。

`Room/TrackPublicationEvent.h` 定义 `room::TrackPublicationSnapshot`、`room::TrackPublishedEvent`
和 `room::TrackUnpublishedEvent`，继续复用 `media::EventBus`。总线同步传播已提交结果，
跨线程消费者复制值快照并投递到所属执行器；注册表锁和房间锁不覆盖网络 I/O 或业务回调。

发布与响应链路为：

```text
WebSocket → WsSignalGateway → 请求通道 → 房间 owner / RoomSignalHandler
  → WebRtcSession 协商 → 注册 MediaStream → Room::PublishTrack
  → 房间 TrackPublishedEvent → Bus 更新版本槽
  → Answer → 响应通道 → 连接执行器 → WebSocket
  → 后续 owner Tick → tracks_changed → 同一响应出口 → 客户端刷新轨道列表
```

当前采用 UTF-8、LF、四空格缩进与 Allman 大括号风格；本轮整理不新增编辑器格式配置文件。
新增或移动文件时需同步维护引用、CMake、独立构建及文档；改名不改变客户端信令字段。

## 统一发布流注册

RTSP 和 WebRTC 的发布流统一进入进程内 `MediaStreamRegistry`。`Room` 继续管理成员、权限和
发布/订阅关系，协议适配层继续持有 SDP、MID、PT、SSRC 等参数；注册表不处理协议协商或媒体包。

```text
RTSP ANNOUNCE/SETUP ── RtspMediaSession ──┐
                                        ├─ MediaStreamRegistry → MediaStream → 通用轨道及 Endpoint 绑定
WebRTC publish ── RoomSignalHandler ──────┘
                         └─ Room/Participant → TrackPublicationInfo.media_track
```

- `scope + name` 定位逻辑流，采用分层索引，不拼接路径。RTSP 保留空 scope 和原 URL suffix；
  WebRTC 使用房间 ID 作为 scope、参与者 ID 作为 name。不同协议不能抢占同一个逻辑流。
- `MediaStream::Source` 保存协议、作用域和发布者信息。`stream_id` 是本次发布的进程内唯一实例 ID，
  取消发布后不复用；`MediaTrackRef {stream_id, track_id}` 定位流内轨道，`track_id = 0` 有效。
- `MediaStream::ReplaceTracks` 为每条 `MediaTrackDescription.ref` 自动赋值；复制描述到新发布流时重新生成引用。
  消费端使用源轨道的 `ref`，无需自行拼接标识或注册另一份源轨道。
- WebRTC 轨道 ID 使用 Endpoint 分配的稳定索引，房间业务轨道通过 `TrackPublicationInfo.media_track`
  引用它。RTSP/WebRTC 产生的编码帧携带 `source.media_stream_id` 和 `source.track_id`，
  可回查相同的内核轨道；原有字符串来源标识保留。
- `FindById`、`FindByName(name, scope)` 查询流，`FindTrack(ref)` 返回通用描述、Endpoint ID、
  可用性状态和消费关系快照；`GetSnapshot()` 同时枚举注册流和全局轨道。快照返回后可能过期，
  不能替代 owner 上的操作前检查。
- `Events()` 发布 `StreamRegisteredEvent`、`StreamUnregisteredEvent`。事件带流快照及注册版本
  `revision`，回调在注册表锁外同步执行；跨线程消费者须复制数据、投递到自己的执行器并处理通知乱序。
  `MediaTrackStateEvent` 报告源轨道暂停、恢复及单轨道移除；整条源流移除通过 `StreamUnregisteredEvent` 报告。
  `revision` 跟踪目录和显式轨道可用性变化，不覆盖消费关系及其暂停状态、所有业务状态或首帧到达。

WebRTC 发布在房间 owner 上建立轨道描述与 Endpoint 绑定、启动媒体并注册，再登记房间发布关系；
失败走原有回滚，先停止传输和 Endpoint，再按对象身份注销。只加入会议或订阅不会新增发布流。
信令断连等待恢复、成功 resume 和 ICE restart 保留流实例；取消发布、离会、恢复到期及停服回收实例。
RTSP 保留原有的接入线程和释放顺序；各协议 owner 负责生命周期，其他模块通过快照和事件观察。
显式 `Clear()` 用于协议资源停止后的统一清理；注册表析构只关闭残留流，不调用业务订阅者。

后续协议可复用 `MediaStream` 的通用轨道描述、注册、Endpoint 绑定及注销接口，无需构造 SDP。
当前接通 RTSP/WebRTC 发布注册框架、受管理消费关系以及进程内轨道控制接口；
RTMP/SRT 媒体接入、跨协议输出、在线新增轨道和跨 SFU 节点目录尚未实现。
媒体热路径不查询注册表，也不会通过全局锁串行转发。

### 全局轨道引用与消费关系

源轨道只注册一次，消费者保存 `MediaTrackSubscription` 句柄。注册表以弱引用记录消费关系，
每条轨道最多 4096 个存活句柄；句柄释放或 `Close()` 后关系失效，过期索引在下次建立关系时清理。

```cpp
auto& registry = media::MediaStreamRegistry::Instance();
const auto ref = stream->GetTracks().front().ref;
auto consumer = registry.AttachConsumer(ref, "recorder");
if (consumer && consumer->CanDeliver()) {
    // 交给自己的有界、非阻塞处理入口。
}
registry.SetTrackPaused(ref, true);                 // 暂停该源的所有受管理消费
registry.SetTrackPaused(ref, false);                // 恢复，不重建关系
if (consumer) registry.SetConsumerPaused(consumer->Id(), true); // 仅暂停此消费者
if (consumer) registry.DetachConsumer(consumer->Id());         // 解除此关系
registry.RemoveTrack(ref);                         // 撤销此源轨道及其消费关系
```

- 源状态是 `Open / Paused / Closed`，消费者另有自己的暂停状态。源恢复不会覆盖消费者的暂停设置。
- `RemoveTrack`、流 `Remove` 和 `Clear` 先关闭原子闸门，再于管理器锁外调用消费端的 `on_close`；
  关闭不可逆，旧句柄和旧回调不会重新绑定同名新发布。析构只关闭闸门，不调用业务回调。
- 闸门阻止后续投递，不等待已进入发送器、网络队列或业务队列的工作；队列取消与排空仍由消费者负责。
  `on_close` 必须非阻塞，通常只投递到消费者 owner；投递失败时闸门仍关闭，资源回收由 owner 的后续检查或 Stop 兜底。
- 房间 RTP 订阅已接入：`Room::SubscribeTrack` 引用业务轨道的 `media_track`，源和目标执行器继续使用现有模型。
  发送出口同时检查闸门，覆盖普通 RTP、缓存重传及发送器 RTCP；暂停不会销毁发送器及重传缓存。
- WebRTC owner 用一个脏标志合并全局事件，在 `Tick` 中核对当前源实例：单轨道移除调用房间取消发布、
  解除下行并移除接收器，随后产生原有 `tracks_changed` 通知；最后一条轨道或整条流移除后关闭该媒体会话，保留会议成员。
- 管理器不直接关闭协议连接，也不修改房间权限。RTSP 尚未接入这套动态单轨道控制的 owner 清理；
  其他协议或业务服务必须显式承接通知。当前没有新增面向客户端或跨节点的轨道控制 API。

### 按流与轨道订阅编码帧

`EncodedFrameRouter` 已归入 `packetia_media_core`，不依赖 RTP、SDP、房间或协议传输。
录制、AI 等现有全量订阅仍可使用 `Subscribe(sink)`；已知发布实例的业务可以直接订阅流或轨道：

```cpp
auto& registry = media::MediaStreamRegistry::Instance();
const auto track = registry.FindTrack({stream_id, track_id});
if (track) {
    const auto result = router->TrySubscribe(sink, track->description.ref, registry, "recording");
    if (result) {
        // 保存 result.id，业务结束时调用 router->Unsubscribe(result.id)。
    }
    // 失败时依据 result.status 返回业务错误，不继续启动消费任务。
}
```

- `ForStream(stream_id)` 选择本次发布的全部轨道，`ForTrack(ref)` 选择一条轨道；流 ID 必须非零，
  轨道 0 有效。同名重发产生新流 ID，旧订阅不会自动跟随。订阅者须自行持有 sink 的共享引用。
- `EncodedFrameFilter` 可同时约束内核流 ID、Endpoint、轨道、原有 session/stream 字符串和 SSRC，
  各条件按 AND 匹配。单独的流内轨道编号不足以订阅，必须同时指定发布实例或 Endpoint。
- 每个路由器默认最多保留 4096 条存活订阅，可通过构造参数 `Options.max_subscriptions` 配置正数上限。
  暂停关系仍占额度；退订、源关闭或 sink 过期后可回收额度。该限制针对关系数量，路由器不缓存帧数据。
- `TrySubscribe` 区分 `Accepted`、`Invalid`、`Full`、`Closed`、`SourceRejected` 和 `Exhausted`，
  分别表示接纳、参数错误、路由满额、路由已关闭、源不可消费及 ID 耗尽。`SourceRejected` 包含源尚未就绪、
  已移除或源侧消费额度不足；失败会撤销本次新建的消费句柄，已有关系保留。内存分配异常继续向调用者传播。
  旧 `Subscribe` 接口保持成功返回 ID、失败返回 0。
- 控制操作串行构建不可变路由快照，发布帧时按发布实例与轨道、整流、Endpoint 或全量订阅查找索引桶，不逐帧复制订阅列表，
  不获取注册表锁或路由写锁；编码帧仍共享只读存储。该设计不意味着所有操作都无锁或已经完成性能压测。
- `Dispatch(event)` 返回 `accepted`、`rejected`、`failed` 和 `examined`：分别表示消费者接收、业务拒绝/队列背压、
  消费者抛出异常及索引选中的候选关系数。异常不阻断其他消费者，路由器不重试或增加缓存。`Publish(event)` 保留原接收数量返回值。
- `GetStats()` 提供活动关系数、容量、关闭状态，以及有效/无效输入帧、接收、拒收、异常和候选检查的累计计数。
  接收等计数以关系投递次数为单位，一帧可投递多次；暂停关系计入活动关系数。指标为并发采样值，尚未接入监控 HTTP 接口。
- 分发仍在发布者线程同步调用，多个媒体 worker 可并发回调同一 sink，不提供跨线程到达顺序保证。
  消费者应迅速投递到自己的有界队列；退订不等待已开始的回调，关闭入口和排空队列仍由消费者负责。
- `Close()` 幂等且不可逆，撤销全部关系并拒绝后续订阅与帧输入；析构时也执行关闭。
  已通过状态检查的回调仍可完成，消费者入口和队列仍需自行收尾。源关闭通知、退订及快照捕获对象释放均不持有路由写锁。
  路由关闭本身不会停止录制、转码或混流任务，相关服务仍按自身生命周期停机。

转码任务已将输入条件交给路由器筛选；`TranscodeSource.media_stream_id` 非零时额外绑定内核发布实例，
原有 Endpoint/轨道匹配规则保留。输出投递异常由转码、混流任务依据 `Dispatch` 结果处理。
验证链路包括注册表轨道引用 → 真实 H264 RTP 分片乱序组帧 → 指定轨道路由 → 有界转码队列 → worker 编码输出。

上面的受管理订阅跟随源暂停、恢复和关闭；可传入非阻塞 `on_close` 回调通知业务服务，主动退订后不再通知。
关闭后的槽位停止投递，路由控制操作会回收失效快照条目。原有 `Subscribe(sink)` 和纯 filter 重载保持独立，
不会隐式绑定注册表；录制、转码等现有旧接口也不会自动停止任务或取消已排队的工作。
编码帧订阅不替代房间权限校验，也不替代直接 RTP 转发。

## 参与者与连接数据结构

`Room` 管理房间成员和跨参与者的轨道转发关系；`Participant` 管理一个参会者的身份、
信令绑定、当前媒体会话、发布轨道和订阅记录。共享监听器、TURN 服务、TLS 上下文和线程池
继续由所属服务或 `ServerApp` 管理，不放进参与者对象。

| 数据结构 | 内容 |
| --- | --- |
| `ParticipantIdentity` | 房间参与者 ID、显示名称、经鉴权确认的用户 `subject_id` |
| `SignalingBinding` | 当前信令连接 ID 和绑定代数 |
| `ParticipantInfo` | 身份、参会状态、信令绑定、发布/订阅会话 ID 和轨道数量的值快照 |
| `ParticipantSession` | 不可变的会话 ID、发布/订阅角色和媒体端点句柄，不含 WebRTC 协议资源 |
| `WebRtcSessionContext` | 服务层私有的 WebRTC 会话、媒体传输、入口、ICE 路由、协商和资源登记状态 |

`ParticipantTypes.h` 不依赖网络或协议实现。参与者 ID 和信令连接 ID 独立，
`BindSignaling` 每次绑定都会增加代数；`UnbindSignaling` 同时匹配连接 ID 和代数，
避免旧连接的迟到关闭通知清除新绑定。解绑信令不等于离开房间。

`RoomTypes.h`、`RoomCommand.h`、`ParticipantTypes.h` 和 `media/track/TrackPublicationInfo.h`
分别定义房间描述、业务命令、参与者描述和轨道元数据；对象接口通过前置声明引用实现。
`TrackPublication.h` 定义轨道对象，调用方按需包含 `TrackPublicationInfo.h` 或 `TrackPublicationFwd.h`。
通用轨道描述保留在 `media/track`，RTP/SDP 参数转换位于 `Rtsp/Rtp`，目录职责见
[RTP 参数与 SDP 适配](#媒体层结构与命名)。房间模型的详细边界见
[房间模型与资源归属](#参与者与连接数据结构)。

媒体创建成功后，服务层创建 `ParticipantSession` 绑定，并通过 `BindPublishSession` 或
`BindSubscribeSession` 交给参与者。两个角色分别持有端点，同一角色不允许覆盖；房间发布和
订阅操作分别选取对应角色端点。`BindEndpoint` 保留给共享双向端点的适配器。
`BindMediaSession` 按角色转发；`GetMediaSession` 在同时绑定两个角色时返回空，
`GetEndpoint` 在存在两个不同角色端点时返回空，调用方应使用明确角色的接口。

`GetInfo()` 在参与者锁内生成值快照，适合状态查询；兼容字段 `media_session_id` 仅在
存在单一媒体会话时有效，双角色调用方使用 `publish_session_id` 和 `subscribe_session_id`。
角色查询返回不可变的绑定，协商和协议操作由服务层在所属 I/O 调度器执行。
`Leave()` 清空信令、会话、轨道和订阅绑定，锁外停止端点、释放会话引用并通知上层；
`WebRtcService::RemoveSession` 继续负责关闭 WebRTC/媒体传输、移除 UDP mux 路由和端点登记。
服务里的会话表保存运行时上下文，上下文持有与参与者共享的房间绑定。

房间参与者的信令意外断开后进入 `Reconnecting`，在恢复窗口内保留参与者及媒体会话；
主动离开或恢复超时才移除房间成员。当前仍使用单条发布或订阅连接。
`subject_id` 来自已验证 JWT 的 `sub`；显示名称来自签名声明，操作权限由成员绑定的鉴权上下文决定。

### WebRTC 房间 owner

`WebRtcService::Start` 选定网络执行器，将其余 I/O 调度器用作房间 owner；只有一个调度器时复用它。
`SignalSessionCoordinator::SelectOwner` 根据房间 ID 选择固定 owner。同一房间的入会、成员更新、
发布订阅、通知和清理在该 owner 上串行推进；多个房间可能共享一个 owner，不为每个房间创建线程。

`RoomOwner` 组合执行器、`RoomSignalHandler`、所属统计及有界管理通道；
`RoomCapacity` 共享服务级人数和媒体额度，避免分片后放大会话上限。
信令请求经通道转给房间 owner，响应回连接执行器统一发送；共享 UDP 入站按当前路由投递，
出站经 `UdpSendQueue` 回网络执行器发送，迟到数据检查连接或路由代次。

网络 owner 不等待房间业务或数据库；媒体处理继续由现有 MediaWorker 执行。
停止时先取消接入和信令任务，再清理各房间及媒体关系，最后取消发送并关闭 UDP 和网络资源。
不同 owner 的房间可以独立推进，但共享网络出口和媒体资源仍可能成为容量瓶颈，需要单独压测。

### 信令恢复与 ICE restart

`webrtc.reconnect_timeout_ms` 默认 15000，允许配置 1..120000 毫秒。断开的参与者仍计入
房间及服务人数限制，定时器到期会统一清理轨道、订阅、端点和 ICE 路由。
未加入房间的连接不能创建媒体会话。

WebSocket 建连后须在 `webrtc.admission_timeout_ms` 内成功 join 或 resume，默认 10000 毫秒；
失败请求不延长期限，成功后取消期限，主动 leave 后重新计时。
`webrtc.signaling_ping_seconds` / `webrtc.signaling_idle_timeout_seconds` 默认 30 / 60 秒，
控制底层 WebSocket 探测与无有效接收超时，超时关闭后才开始参与者恢复窗口。
服务端超时通知使用默认 250 毫秒的限时排空，慢客户端到期强制断开。
接入与恢复计数、处理耗时及实时状态可通过 `WebRtcService::GetStats()` 查询；
队列排空结果通过 `WsServer::GetStats()` 查询。详见[信令生命周期](#信令恢复与-ice-restart)。

加入房间的 `joined` 回复增加 `resume_token` 和 `reconnect_timeout_ms`。恢复凭据是服务端生成的
256 位随机值，只对应当前房间中的当前参与者，不由参与者 ID 推导；客户端须同时提供仍有效、
身份、房间和绑定代次与原成员匹配的房间 JWT。凭据保存在客户端内存中，线上经反向代理提供 WSS，
不放在 URL 或日志里。

新 WebSocket 建连后发送：

```json
{
  "type": "resume",
  "token": "<valid-room-jwt>",
  "room_id": "demo",
  "participant_id": "<original-participant-id>",
  "resume_token": "<resume-token-from-joined>"
}
```

服务端校验 JWT 身份、入房授权、房间绑定代次、恢复凭据、断线状态和截止时间，再把成员及会话索引迁移到新连接，增加信令绑定代数。
`resumed` 回复的 `ice_restart` 表示是否存在需要恢复的媒体连接。仍在线的参与者不能被新连接接管；
错误凭据、跨房间恢复、主动离开后的恢复和过期恢复都会拒绝。恢复凭据在参与者离开时失效。

有媒体连接时，浏览器保留原 `RTCPeerConnection`、本地轨道和收流对象，调用
`createOffer({iceRestart: true})` 并等待新一轮 ICE 候选收集完成，再发送
`{"type":"restart_ice","sdp":"<new-offer>"}`，此操作使用连接已绑定的权限。
服务器生成新的本地 ICE ufrag/password，回复 `answer`；浏览器调用 `setRemoteDescription`，
随后作为 Full ICE 端发起检查和提名。服务器继续使用 ICE-Lite，不主动枚举浏览器的网卡。

此次 restart 仅允许改变 ICE/网络参数，协商后的轨道、MID、PT、方向、SSRC、扩展、DTLS 指纹和角色
保持不变。服务端保留 SFU 端点、发布轨道、订阅关系及已建立的 DTLS/SRTP 状态；原 ufrag 和地址路由
被撤销，新路径完成认证和提名前暂停媒体收发。协商失败保留原协商与路由。

核心 C++ 已接通 `WebRtcSession → WebRtcMediaTransport → SendSideController` 的路径状态通知：
restart 等待期间禁用发送控制；新路径可用后清除旧反馈历史和带宽估计，保留 TWCC 分配器及会话身份。
同代次地址重绑定也触发路径更新，重复提名不重复重置；旧路径排队的发送任务按 epoch 作废。
阅读顺序、测试边界与关键帧/统计的后续任务见[服务端切网恢复框架](#信令恢复与-ice-restart)。

`examples/webrtc-room` 自动重连信令，按指数退避重试恢复，再完成 ICE restart；ICE 状态变成 `failed`
且信令仍在线时也会尝试 restart。恢复超时、已终止的媒体会话、页面刷新或服务端重启需要重新加入房间。
目前没有持久化恢复、跨节点迁移、TURN/TCP 或 TURN/TLS 回退。

## 双浏览器房间与 TURN 示例

`examples/webrtc-room` 使用实际 Room/SfuEndpoint 转发链路：两个浏览器加入同一房间，
A 发布，B 订阅，并可在 B 建连前勾选 Force TURN 强制 `iceTransportPolicy=relay`。
WebSocket 信令支持 join、tracks、publish、subscribe、resume、restart_ice、leave；
每个参与者当前只承担发布或订阅一种角色，房间内轨道变更重协商尚未开放。
订阅通过轨道 ID 和下游 MID 显式绑定。
房间轨道发布、移除通过 EventBus 接到信令层，使用 `tracks_changed` 主动推送最新列表；
客户端需将该通知与请求响应分流，示例页面已适配。通知不会自动创建或重协商订阅。

需要安装上述 WebSocket/libSRTP 构建依赖及 Node.js。首次运行先配置构建目录；
macOS 按上面的构建说明配置 OpenSSL/libwebsockets 路径，或通过 `PACKETIA_ROOM_DEMO_BINARY`
使用已有构建产物。

```sh
cmake -S . -B build/webrtc-integration -DBUILD_TESTING=ON
cmake --build build/webrtc-integration --target PacketiaRoomDemo
cd examples/webrtc-room
npm ci
npm start
```

打开 `http://127.0.0.1:18000`，两个浏览器使用相同房间名，A 选择 Publisher 后发布，
B 选择 Viewer 并勾选 Force TURN 后订阅。启动器生成临时 API key/secret 与 TURN 凭证并启动 C++ 示例；
回环地址的开发签发接口按角色生成各自 JWT，浏览器只在 join/resume 时携带 JWT。
恢复时开发签发端提供同一身份和房间的新 JWT；生产签发端需要接入登录和房间授权。
默认 WebSocket 18080、SFU UDP 19000、TURN UDP 13478，均用于本机演示。
其他构建目录通过 `PACKETIA_ROOM_DEMO_BINARY` 指定二进制。
示例 TURN 只允许转发至该 SFU 的回环媒体地址。

启动示例后运行 `npm run check`，需要本机 Google Chrome；检查两个独立浏览器中的视频帧增长、
音频流量、订阅端选中的 relay 候选、视频尺寸、双方信令断线后的参与者恢复和 ICE restart、
主动退出清理，并在忽略的 artifacts 目录输出截图。

## WebRTC SDP 协商

房间发布/订阅的运行入口与限制见上面的双浏览器示例。服务器在客户端 offer 中选择本地支持的
H264/Opus 能力，answer 沿用选中 codec 在 offer 中的 PT；本地能力声明中的 96/111 不会强制覆盖
客户端的动态 PT。协商支持媒体方向、RTCP feedback 和 RTP header extension，并校验
BUNDLE、ICE、DTLS、SSRC 与 m-line。Offer/Answer 状态通过提交和回滚管理。

普通 re-offer 不允许直接更换 ICE 凭据；同会话换路径使用上述 `restart_ice` 入口。
当前 restart 保持有效协商的媒体参数及 DTLS 身份不变，不同时进行增删轨道等媒体变更。

## RTP 接收排序与弱网恢复

音频等通用接收路径使用 [RtpPacketReorderQueue](Rtsp/RtpPacketReorderQueue.h)，按展开后的序号排序，
处理 `65535 → 0` 回绕、重复包、迟到包、容量推进和缺包等待超时。
默认向前窗口为 1000，缓存最多 50 包、等待 100 ms；正常连续包立即输出。
期限从被阻塞包的本机单调时钟到达时间起算，新包不会延长旧缺口的期限。

前向超窗恢复要求正常输出停滞至少 500 ms、旧队列为空，且在严格小于 100 ms 内确认三个连续候选包。
前两包丢弃，第三包起向前推进，保留累计统计。后退重启或恰好半周期的歧义序号不能据此恢复，
需要会话层重置；包进入队列前仍需验证源身份、SSRC、PT 及适用的鉴权。

H264 直接使用独立帧缓冲处理帧内乱序，不在它之前叠加通用排序器。
默认组帧等待 100 ms，超时丢弃不可用的依赖链，保留参数集并等待恢复关键帧；
停止旧缺口 NACK 重试并请求关键帧。现有媒体 worker 约每 10 ms 检查已登记流，停包后也会检查。
这些缓冲作用于接收组帧，原始 SFU RTP 转发继续收到即转发。

**100 ms 是缓冲等待预算，实际延迟还受 worker 排队、回调阻塞和 socket 积压影响。**
Apple M5 / macOS 26.5.1 上完成修改前后各 72 次试验，包含确定性故障模拟和真实回环 UDP：

| 注入场景 | 测量结果 |
| --- | --- |
| 1000 包/秒、10–90 ms 延迟、3% 丢包、2% 重复 | 默认配置输出率 97.14%，未额外丢弃唯一源包 |
| 1000 包/秒、20–200 ms 延迟、10% 丢包、5% 重复 | 基线默认配置输出率 59.10%；改为 128 包/200 ms 后 90.08%，等待增加 |
| 5000 包/秒、10–50 ms 延迟、1% 丢包 | 基线容量 50→256，输出率 71.05%→98.97%；排队 P95 26.51→63.46 ms |
| 接收 owner 阻塞 250 ms | 排队最高 270.65 ms，包数上限不能约束线程阻塞造成的延迟 |
| 连续丢失 1200 包 | 基线 3/3 未恢复；前向重同步后 3/3 恢复，每轮输出 800→1798 包，仅额外丢两个探测包 |

输出率包含注入的网络丢包。上述故障参数用于边界验证，尚未按真实业务网络分布校准；
试验使用合成 RTP 数据，未测量视频冻结、完整转码/SFU 播放链路、公网表现或并发容量。
策略与统计口径见[排序队列设计](#rtp-接收排序与弱网恢复)，原始数据见
基线报告（历史记录，报告文件未随仓库保留）与
重同步后报告（历史记录，报告文件未随仓库保留）。

在已配置 `BUILD_TESTING=ON` 的 macOS/Linux 构建目录复现：

```sh
cmake --build build --target test_rtp_reorder_queue rtp_weaknet_bench -j4
ctest --test-dir build --output-on-failure \
  -R '^(rtp_reorder_queue|rtp_weaknet_smoke|rtp_resync_smoke)$'
python3 Rtsp/Test/run_rtp_weaknet_bench.py \
  --binary build/Rtsp/Test/rtp_weaknet_bench \
  --output build/rtp-weaknet-results
```

脚本默认运行 72 次试验并写出环境、逐次统计和采样数据，不修改系统网络配置；
真实 UDP 部分仅向本机回环发送。

## 弱网与带宽控制

下行音视频和重传共用每条 Transport 的 `SendSideController`，独立维护 TWCC 序号、
发送历史和带宽估计。参考 BWE 代码的 OU 时延检测与 RemoteRateControl 核心算法已适配到
`media/quality/bwe`，使用项目现有类型和控制接口，无需旧工程的配置及 trace 依赖。

```text
RtpSenderTrack 发送成功 → SendSideController 记录发送历史
观众 RTCP TWCC → 匹配发送历史 → TransportBweController
  → 时延趋势 + 接收速率 + RTT + 丢包 → 目标码率 / Pacer 配置建议
观众 RTCP RR → TWCC 有效时补充 RTT；TWCC 过期后驱动 RR 回退
```

码率由 `BitrateConstraints` 统一限制。正常启动使用配置的初始码率，接收速率有效后观察约
2 秒初始化估计；持续排队会提前降码率，轻微排队或丢包观察期间保持，路径稳定后逐步恢复。
升码率按经过的时间和 RTT 推进，避免反馈频率越高、增长越快。
接收速率与目标码率分别输出；估计尚未初始化时 `bandwidth_bps` 为 0，目标码率仍有效。
网络断开或显式路由变化事件会清空旧估计；完全没有反馈时的周期超时处理仍待实现。

SRTP 发送端允许重新保护缓存的原 RTP 序号及 payload，重传重新分配 TWCC 序号；
接收端继续执行认证和防重放。当前协商的保护配置为 `SRTP_AES128_CM_SHA1_80`。

视频 NACK 在乱序等待后发起，按 RTT 重试；最后一次请求仍保留响应时间，超过恢复期限则
回退到 PLI。下行重传默认限制原包年龄为 500 ms，发布轨道合并不同订阅者的 PLI 请求。
WebRTC 会话的 100 ms 定时器向媒体池投递恢复任务，停止收包后仍能推进重试与超时。

业务可通过 `SetUpdateCallback` 消费控制输出。订阅管理器使用独立观察者获取预算，不覆盖业务回调；
通知合并后回到订阅端媒体 Worker 读取最新状态，周期恢复任务继续推进分配和超时。

### 指标采样与平滑

`NetworkStatsCollector` 已接入上行每个 SSRC 的 `WeakNetController` 和下行每条 Transport 的
`GoogCcNetworkController`，对实际收到的 RTT、丢包率、jitter、收发码率样本进行 EWMA 平滑。
接收侧在 `GetOrCreateReceiverTrack()` 建立实际接收轨道时，通过 `RegisterSource(ssrc)` 同步创建评估器，
覆盖 RTP 先到和 SR 先到的入口。注册本身不产生样本或发送 RTCP；尚无反馈时质量为 `Unknown`。
反馈只更新已注册来源，重复注册保留历史，删除接收轨道及端点停止会清理对应状态。
`GetReceiveQualitySnapshot(ssrc)` 可读取状态，尚未创建接收器或已移除时返回 `nullopt`。
接收侧评估和 RR 发送当前仍由 SR 触发，创建时机独立于评估周期；公共 Tick 尚未接入周期质量评估。

默认时间常数 1000 ms，权重按 `exp(-采样间隔 / 时间常数)` 计算；重复、倒序及非有限样本不参与更新。
每个指标独立记录新鲜度，超过 3000 ms 返回 `nullopt`，下一个有效样本重新初始化该指标。
RTT-only 事件只更新 RTT，TWCC 只更新确认接收码率和反馈丢包率，不刷新其他指标的年龄；
旧接口中为 0 的 RTT/码率按缺失样本处理。断网、路由变更及接收源移除会丢弃对应滤波历史。

控制输出的 `target_rate.estimate.smoothed` 和上行 `NetworkStatsSnapshot.smoothed` 保存平滑值，
与原始字段同时提供。jitter 在 RTP 层原有的 RFC 3550 计算及 TWCC 排队趋势算法继续使用原始采样；
质量降级、NACK/PLI 和拥塞控制继续读取原始反馈，平滑结果用于观察指标趋势。

订阅需求的 `SourceBitrateEstimator` 也复用 `EwmaFilter`，以约 1750 ms 时间常数处理至少 500 ms 的码率样本，
常规间隔下新样本权重约 25%。源停止发包期间保留最后一次需求码率，超过 3000 ms 的采样空档后重建滤波历史，
避免观众暂停或源静默将需求错误测成零。通用平滑器编入 `media_quality`，无需调用方单独链接。

### 将带宽估计落实到订阅

`DownlinkBandwidthAllocator` 为每条 Transport 分配预算，同一连接的音视频竞争同一预算，不同观众互不影响。
默认使用目标码率的 85%，预留重传和传输开销；先保留音频及显式豁免轨道，再按优先级从高到低准入视频。
同优先级优先保持当前转发状态，并按订阅 ID 确定顺序。放不下的大轨道跳过，剩余预算可用于较小轨道。
单条需求来自发布源 RTP 字节速率的平滑估计，暂停某个观众不会把需求测成零；无有效样本时使用业务提示，
仍未配置时视频按 500 kbit/s、音频按 32 kbit/s 估算。

预算持续不足 300 ms 才暂停视频；恢复要求多留 20% 余量且持续满足 1000 ms，减少反复开关。
仅初始化完成且最近 1500 ms 内有有效 TWCC 的估计用于自动分配；启动阶段、反馈过期及 RR-only 连接冻结原分配，
不将初始目标码率当作已测容量。音频始终保留，预算不足时无法保证所有保留轨道的总需求都能满足。

订阅建立后，可通过 C++ 接口调整策略并读取带宽状态：

```cpp
media::DownlinkSubscriptionPolicy policy;
policy.priority = 200;              // 0..255，数值越大越优先
policy.bitrate_hint_bps = 800000;   // 发布源速率尚未测出时使用
viewer->SetSubscriptionBandwidthPolicy("camera", policy);
auto status = viewer->GetSubscriptionBandwidthStatus("camera");
// status: state、policy、required_bps；未知或已关闭的订阅返回 nullopt。
// policy.enabled = false 可豁免该订阅的自动带宽暂停，其需求仍从预算中扣除。
```

带宽状态独立于用户暂停和发布源暂停：`Forwarding` → `Paused` → `WaitingKeyFrame` → `Forwarding`。
带宽暂停保留订阅身份和发送器，阻止媒体及缓存重传；恢复时每秒最多请求一次关键帧，等待完整的 H264 IDR 或
H265 IDR（19/20，不接受 CRA）后再打开媒体出口。恢复缓冲支持常用单 NAL、聚合及 FU 分片、序号回绕和乱序，
单帧最多 512 包、2 MiB、200 ms，缓存并重放暂停期间的参数集。恢复时清空旧重传缓存、重建下游序号映射，
跳过有意丢弃的区间，保留后续真实网络丢包形成的序号缺口；RTP 时间戳保留源时钟的时间间隔。
路径切换清理旧预算及恢复缓存，活动视频重新等待关键帧，原本暂停的视频继续等待新路径预算。

为使视频暂停后仍能探测恢复空间，管理器选择最高优先级且协商 TWCC 的暂停视频发送纯 padding：
每 3 秒最多一个 500 ms 探测簇，每簇计费上限 128 KiB，单次 Tick 突发上限约 16 KiB。
探测总目标受控制器最大码率及当前目标的增长上限约束，并扣除活动媒体需求；padding 使用新 RTP/TWCC 序号，
不进入 NACK 缓存。用户暂停或源暂停期间不发送探测；最后一条订阅移除时注销对应观察者。

当前执行的是单编码视频的整轨暂停/恢复，尚未接入 simulcast/SVC 选层、编码器动态调码率或实际 Pacer；
受限 padding 探测也不等同于完整的探测与 pacing 系统。策略接口目前仅提供 C++ API，未新增信令字段。
组件及 Worker 集成测试不代表真实浏览器弱网质量已经验收。

## 本轮验证

2026-11-04 指标平滑接入验证（macOS）：新增 9 个用例通过，覆盖按时间加权、异常与重复采样、
指标独立过期、RTT-only/TWCC 事件隔离、切网和接收源清理、源码率静默保留，以及原始拥塞反馈立即生效。
`Packetia` 及相关测试目标构建通过；下述相同 6 组 CTest 回归全部通过，本轮未进行真实浏览器弱网验证。

2026-11-04 订阅带宽策略验证（macOS）：`Packetia` 及相关测试目标构建通过；
`media_core`、`test_webrtc_crypto`、`test_weak_network_manual`、`test_rtp_input`、
`test_udp_binding`、`webrtc_service` 共 6 组 CTest 全部通过。新增 19 个用例覆盖预算分配与滞回、
反馈过期、用户/源暂停隔离、关键帧组装和参数集重放、RTP 序号及时间戳回绕、padding、观察者回收、
切网，以及实际 Worker 上由 TWCC 反馈触发的订阅暂停/恢复和观众隔离。
本轮未进行真实浏览器弱网或公网验证，构建仍有既有重复库链接警告。

2026-10-29 提交前验证（macOS）：当前 `build/webrtc-integration` 配置完整构建通过，包含
`Packetia`、`PacketiaControl`、`PacketiaSfu` 和全部测试目标；**67/67 组 CTest 通过，无跳过**，
总耗时约 104 秒。覆盖会议服务生命周期、鉴权、成员管理、控制面与双 SFU 集成、房间 owner、
信令通道、全局轨道与编码帧路由，以及 RTP、WebRTC、TURN、网络、转码、混流和监控回归。
修改的浏览器 JavaScript 和 Python 脚本通过语法检查，README 本地文件链接及暂存区 diff 格式检查通过。
本次没有重跑真实浏览器媒体互通，也未进行公网、商业容量或长稳验证；构建仍有既有重复库链接警告。

2026-10-23 服务端切网框架验证：`Packetia` 及相关测试目标构建成功，六个 CTest 入口通过，
覆盖会话、媒体适配、发送控制、业务服务、DTLS/SRTP 和 UDP mux。
核心已接通路径状态通知、旧估计重置和旧队列发送作废；恢复关键帧、耗时统计及真实手机切网仍待完成。
改动清单、复现命令和验证边界见2026-10-23 变更说明（历史记录，报告文件未随仓库保留）。

2026-10-21 多编码与接收恢复验证（Apple M5 / macOS 26.5.1）：完整构建成功，
现有 `build/webrtc-integration` 配置下 **54/54 组 CTest 通过，无跳过**。
包含 2 个任务领域、18 个视频转码、19 个多编码和 24 个排序队列用例；
多编码测试覆盖五种输入各生成五种输出的 25 种组合。
24 个队列用例和 23 个 H264 组帧/期限用例另有 ASan/UBSan 验证。
本机 FFmpeg 未提供 libaom，相应候选尚未实测；软件计算验证不代表五种格式的浏览器互通或时延验收。
完整改动与两轮弱网报告入口见当天变更说明（历史记录，报告文件未随仓库保留）。

```sh
cmake --build build/webrtc-integration -j4
ctest --test-dir build/webrtc-integration --output-on-failure --timeout 120
```

以下保留各日期当时的验证结果和限制。

2026-10-15 控制服务与 SFU 拆分验证（macOS）：`PacketiaControl`、`PacketiaSfu` 和原 `Packetia`
均编译通过；13 个相关 CTest 入口通过，包含一控两 SFU 的真实进程故障测试及原有账号、鉴权、
HTTP、WebRTC 回归。本地启动脚本连续启停两轮，账号与密钥正常保留。Docker Compose 配置已提供，
当前环境无 Docker，尚未验证容器构建；复现命令与边界见[会议控制与节点调度](#会议控制与节点调度)。

2026-10-15 C++ 账号业务验证（macOS）：`Packetia` 编译通过，
`AccountBusinessUnitTest`、`AccountFrameworkUnitTest`、`AccountApiFrameworkUnitTest`、
`ApiRouterUnitTest`、`server_config`、`server_app` 六个 CTest 入口全部通过。
真实 `Packetia` 进程的 HTTP 联调通过：注册 201、重复注册 409、登录/查询/续期 200、
旧 token 401、退出 204、撤销后查询 401；进程重启后有效会话仍可使用，关闭注册返回 403。

2026-10-14 HTTP 接入、API 路由与线程管理重构验证（macOS）：
`Packetia` 在 libwebsockets 开启、关闭两种构建配置下均编译通过。
以下 11 个 CTest 入口通过，覆盖真实 TCP HTTP 请求、账号 API 框架、线程停止与回收、
工作队列排空/丢弃、I/O 异常清理、配置校验、AI/混流线程登记和服务失败回滚、重复启停。
独立构建的 `WorkerRegistryTest` 也通过。

```sh
cmake --build build/webrtc-integration --parallel 4 --target \
  Packetia test_server_app test_http_server test_account_api \
  test_thread_manager test_worker_registry test_kqueue_scheduler \
  test_sharded_worker_pool test_sharded_worker_lifecycle \
  test_server_config test_stream_services conference_mix_tests
ctest --test-dir build/webrtc-integration --output-on-failure \
  -R '^(server_app|HttpServerIntegrationTest|AccountApiFrameworkUnitTest|ThreadManagerTest|WorkerRegistryTest|KqueueSchedulerTest|ShardedWorkerPoolUnitTest|ShardedWorkerLifecycleTest|server_config|stream_services|ConferenceMixFramework)$'
cmake --build build/webrtc-disabled --target Packetia --parallel 4
```

上述 2026-10-14 验证使用替代存储/密码适配器，验证当时的框架调用与错误处理；
该历史验证不包含实际注册登录、浏览器弱网或容量压测。账号业务的新增测试位于 `service/Account/test`。

2026-10-13 鉴权重构验证：`Packetia` 和 `PacketiaRoomDemo` 构建通过，鉴权库 20 项 GTest 通过，
`AuthFrameworkUnitTest`、`server_config`、`server_app`、`webrtc_service` 四个 CTest 入口通过。
真实 Chrome 双浏览器使用不同房间 JWT：发布者直连、观众经 TURN relay，音视频正常接收；
双方信令断开后重新获取同身份/权限 JWT，原参与者恢复并完成 ICE restart，视频继续增长，退出清理通过。

```sh
cmake --build build/webrtc-integration --target Packetia PacketiaRoomDemo test_auth_framework test_server_config test_server_app test_webrtc_service --parallel 4
ctest --test-dir build/webrtc-integration --output-on-failure -R '^(AuthFrameworkUnitTest|server_config|server_app|webrtc_service)$'
# 运行双浏览器验证前，另一个终端先启动 examples/webrtc-room 的 npm start
cd examples/webrtc-room
npm run check
```

2026-10-12 本轮弱网验证：主程序编译通过，33 项弱网测试、27 项 RTCP/SFU 回归、
WebRTC 会话与 DTLS/SRTP 测试通过。新增 15 项带宽算法测试覆盖启动约束、反馈频率、RTT、
丢包与排队交叠、时钟回绕、路径重置及数值边界；真实 TWCC 字节验证降码率和恢复链路。

```sh
cmake --build build/webrtc-integration \
  --target Packetia test_weak_network_manual test_rtp_input test_webrtc_session test_webrtc_crypto --parallel 4
ctest --test-dir build/webrtc-integration --output-on-failure \
  -R '^(test_weak_network_manual|test_webrtc_session|test_webrtc_crypto)$'
build/webrtc-integration/Rtsp/Test/test_rtp_input \
  --gtest_filter='RtcpChainTest.*:SfuConference.SubscriberOwnsSenderAndFeedbackReturnsToPublisher:SfuConference.RebuildsDownstreamMidAndTwccInsteadOfCopyingUpstreamIds'
```

2026-10-10 已完成主程序和房间示例构建，以下六个测试套件及 17 项房间/媒体测试通过：

```sh
cmake --build build/webrtc-integration --parallel 4
ctest --test-dir build/webrtc-integration --output-on-failure \
  -R '^(test_webrtc_session|test_webrtc_crypto|test_datagram_transport|webrtc_service|server_config|server_app)$'
build/webrtc-integration/Rtsp/Test/test_rtp_input \
  --gtest_filter='Participant.*:SfuConference.*:SfuRoomCommands.*:WebRtcMediaAdapter.*'
```

验证覆盖原参与者/会话恢复、发布与订阅关系保留、错误或过期恢复凭据拒绝、显式离开清理、
新 ICE 凭据与提名、旧路径撤销以及协商失败回滚。TLS 组件测试入口见网络章节。
真实 Chrome 双浏览器检查通过：订阅端保持 relay 候选，双方恢复后视频继续解码，参与者 ID 不变。
上述结果不代表全量测试无失败：已有 `RtpPacketPoolTest.RejectsOversizedPacketBeforeAllocation`
超大包计数失败仍待修复。

## 开发记录

以下记录描述各次修改当时的状态；其中的待实现能力和限制可能已在后续完成，当前状态以上文为准。

### 2026-10-30：整理帧分发与接收质量的模块边界

- 编码帧分发迁入 `media/frame` 和 `packetia_media_core`，解除对 `SfuEndpointTypes.h` 的依赖；帧回调重入测试归入媒体核心测试。
- `media/quality/ReceiveQualityMonitor` 仅接收统计、保存各源控制器并返回控制建议。
- `media/endpoint/ReceiveFeedbackHandler` 衔接 RTP 接收器、RR 发送和质量评估，保持原有反馈与关键帧路径。
- 端点类型头删除质量状态和帧回调声明；质量模块新增多源隔离、移除与清空后的状态重置测试。
- `Packetia`、`PacketiaControl`、`PacketiaSfu` 构建通过；媒体核心、弱网、RTP/组件、RTSP UDP、流服务和 WebRTC 共 6 组 CTest 全部通过。

### 2026-10-30：拆分 SFU 端点职责

- 下行参数、订阅和发送器生命周期移入 `DownlinkTrackManager`，端点保留源/目标加锁与跨端点通知。
- 编码帧来源、回调和发布交给 `EncodedFrameDispatcher`；接收报告与质量评估交给 `ReceiveQualityMonitor`。
- WebRTC 显式传入帧来源，删除只为修改内部字段而存在的 `PublishedEndpoint` 派生类。
- 补充组件销毁后旧路由失效、RTCP 注册撤销、帧回调重入和实际音频收包后 RR 身份的回归测试。
- `Packetia`、`PacketiaControl`、`PacketiaSfu` 构建通过；媒体核心、RTP/组件、RTSP UDP、流服务和 WebRTC 共 5 组 CTest 全部通过。
- 保留现有客户端协议与执行器归属；发布协商和接收器管理仍由端点协调。

### 2026-10-30：编码帧转发准入、索引与关闭

- 编码帧路由增加可配置订阅上限、明确准入结果和失败回滚，保留旧订阅接口。
- 按发布实例与轨道建立独立索引；同一流 256 条轨道的测试中，单帧仅检查目标轨道及三条通用关系，共 4 个候选。
- 新增幂等关闭与累计投递统计；旧快照会检查关系是否失效，控制路径在锁外释放回调捕获，允许重入。
- 增加满额、过期回收、回调异常、关闭并发、失败清理及析构重入测试。
- `Packetia`、`PacketiaControl`、`PacketiaSfu` 构建通过；媒体核心、RTP、WebRTC、转码、混流和流服务 6 组 CTest，以及独立转码构建的 3 组 CTest 全部通过。
- 本次范围为编码帧路由，不改变直接 RTP 转发算法、消费者队列策略或客户端信令；未做容量与长稳压测。

### 2026-10-29：媒体层目录与轨道命名统一

- 区分源描述、发布信息对象和消费句柄，更新为 `MediaTrackDescription`、`TrackPublicationInfo`、`TrackPublication` 与 `MediaTrackSubscription`。
- 将帧类型和编码帧路由集中到 `media/frame`，将质量测试统一为 `media/quality/tests`，规范 BWE 文件名和相关头文件保护宏。
- 房间发布事件移至 `Room/TrackPublicationEvent.h` 与 `room` 命名空间；同步更新房间、信令、RTSP、录制、转码和混流引用。
- 更新主工程与独立构建的 CMake 源文件和头文件路径；媒体层规范集中记录在本 README。
- 目录命名检查覆盖 `media` 下 102 个 C++ 文件；三个主程序和 9 组相关 CTest 通过，独立转码 3 组、混流 1 组测试通过。
- 尚未验证商业容量、长稳、公网部署或新增协议互通；这些结果不作为生产就绪结论。

### 2026-10-29：全局轨道引用与消费控制

- 源流为 `MediaTrackDescription.ref` 赋予进程内全局引用，描述克隆到新发布时更换实例身份；消费者不重复注册源轨道。
- 注册表增加轨道索引、消费关系快照、源与单消费者暂停/恢复、解除消费、单轨道移除和可用性事件。
- 消费句柄使用原子闸门与弱引用登记，关闭先停止后续投递，再于管理器锁外通知清理；每条源轨道限制 4096 个存活句柄。
- 房间 RTP 订阅和受管理编码帧订阅接入控制；RTP 发送出口覆盖缓存重传，暂停保留发送器与消费关系。
- 全局移除通过脏标志回到 WebRTC owner，在原有房间链路中清理接收器、订阅与媒体会话并通知客户端，成员身份保留。
- 补充引用生成、未就绪拒绝、暂停隔离、关闭重入、容量限制、并发移除、真实 RTP/NACK 及 WebRTC owner 清理测试。
- `Packetia`、`PacketiaControl`、`PacketiaSfu` 构建通过；媒体核心、RTP、RTSP UDP、WebRTC、转码、混流及流服务 7 组 CTest 全部通过。
- RTSP 动态单轨道 owner 清理、旧全量帧订阅的任务联动、动态新增轨道、跨节点控制和容量压测仍未实现。

### 2026-10-29：编码帧按流与轨道路由

- 将编码帧路由归入 `packetia_media_core`，新增按发布实例、轨道或 Endpoint 等来源条件订阅，保留原有全量订阅。
- 使用不可变路由快照和来源索引；逐帧分发不再复制全部订阅列表，不持有路由写锁调用消费者。
- 新增接收、拒收、异常的投递结果，隔离消费者异常；退订使旧快照条目失效，已开始的回调仍由消费者入口和队列收尾。
- 转码任务接入来源过滤和内核发布实例 ID；真实 RTP 分片乱序组帧经过指定轨道路由，完成 12 帧输入到两档共 24 帧输出的验证。
- 清理已删除媒体测试的失效 CMake 引用，新增独立 `media_core` 测试；三个服务入口构建及 8 组相关 CTest 通过。
- 未新增全局轨道控制命令、源注销自动停止业务、跨协议输出或容量压测。

### 2026-10-29：统一发布流注册框架

- RTSP/WebRTC 发布统一进入 `MediaStreamRegistry`，按 scope/name 查找逻辑流，按发布实例 ID 和流内轨道 ID 解析轨道及 Endpoint。
- 补齐来源信息、全局值快照及注册/注销事件；房间业务轨道、编码帧关联同一内核发布实例，原有客户端信令字段保持不变。
- WebRTC 发布失败、取消发布、离会、恢复到期及停服注销流，resume/ICE restart 保留实例；订阅不重复注册发布流。
- 三个服务入口构建通过，9 组相关 CTest 通过，包含多 owner 注册并发、旧发布清理隔离、注册冲突回滚、RTSP UDP 收帧溯源和一控两 SFU 集成。
- 本次仅打通进程内注册框架；未实现 RTMP/SRT 接入、跨协议输出、全局轨道控制命令或跨节点流目录，未新增浏览器、公网和容量验证。

### 2026-10-29：会议控制面统一服务生命周期

- 新增 `MeetingModule : ServiceBase`，内部组合会议业务、准入、管理和共享节点路由；`ControlApp` 将会议和监控注册到 `ServiceManager`。
- 补齐 `ServiceBase` 生命周期实现和 `ServiceManager::MakeKey`，拆出独立的 `packetia_service_core`，新增 `ServiceType::Meeting`。
- 公共会议和内部节点 API 均经过模块准入检查；停止时拒绝新操作，等待已接收操作完成，HTTP worker 排空后再释放数据库。
- 数据库从构造阶段移至 `Init`，失败自动回滚；支持 `Stop → Start` 和 `Shutdown → Init → Start`，健康检查报告会议模块状态。
- 三个运行入口构建通过，11 个相关 CTest 入口通过，包含生命周期失败回滚、停止排空、旧路由保护及一控两 SFU 集成；未新增公网或容量验证。

### 2026-10-29：会议业务与节点调度拆分

- 将混合的 `ClusterController` 拆为公共 `RoomController`、内部 `NodeController` 和 SFU 的 `RoomControlController`。
- 抽出 `MeetingService`、`RoomAdmissionService`、`RoomAdminService`；`Coordinator` 只管理节点租约和房间绑定。
- 后台成员管理经过目标 SFU 的有界 owner 通道，复用 WebSocket 的资料校验、撤权、媒体回收与通知。
- 增加房间绑定代次、空房间回收与 `closing → ended` 流程；长期角色在下一次新入会生效，重连保留会内权限。
- 修复后端踢出最后一人时房间索引被删除导致的悬空引用；覆盖排队命令超时和迟到回收隔离。
- `Packetia`、`PacketiaControl`、`PacketiaSfu` 构建通过；9 个相关 CTest 入口全部通过，包含一控两 SFU 的真实进程测试。
- 本地启动脚本验证可生成 64 个节点的不冲突端口配置；这不代表已完成 64 节点运行或容量测试。

具体接口、线程归属、升级和失败语义见[会议控制与节点调度](#会议控制与节点调度)。

### 2026-10-23：服务端切网路径状态与媒体控制恢复

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
对齐真实 socket 完成回执。阅读顺序及接入契约见[服务端切网恢复框架](#信令恢复与-ice-restart)，
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

### 2026-10-21：多编码转码与 RTP 弱网恢复

- 新增五种视频格式的软件适配、公共配置与接口、实现工厂；分开格式、具体库、码流表示和传输 PT。
- 接通完整编码帧输入、一次解码、多档缩放编码和独立输出路由，提供任务启停、状态、准入及过载恢复。
- 重构通用 RTP 排序队列，补齐 H264 组帧期限、媒体 worker 空闲检查和长断流前向重同步。
- 完成修改前后各 72 次本机弱网试验，保留 CSV、JSONL、缓存采样及环境；完整构建和 54 组 CTest 通过。
- 整理格式兼容规划、实际适配边界与验证数据；下一步优先打通转码输出的协议适配和播放链路。

详细清单及实测结论见2026-10-21 变更说明（历史记录，报告文件未随仓库保留）。

### 2026-10-14：HTTP 接入、API 路由与统一线程管理

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
第三方编解码库内部线程通过各库接口管理。测试范围与复现命令见[本轮验证](#本轮验证)。

### 2026-10-13：房间 JWT 与参与者鉴权

- 按 LiveKit 的职责划分加入 API key provider、HS256 JWT 验证与 room grants 授权。
- 替换 WebRTC 共享 token，绑定可信用户身份与房间权限，授权后再创建媒体资源。
- 恢复校验 JWT 身份、原权限和随机恢复凭据，关闭旧独立 offer 的鉴权绕过入口。
- 迁移配置、签发工具及双浏览器示例，验证观众经 TURN 与双方重新鉴权恢复。

### 2026-10-12：集成发送侧带宽算法与修复 SRTP 重传

- 将参考 OU 时延检测、接收速率与 RemoteRateControl 码率算法适配到现有 TWCC 链路。
- 统一启动码率约束，补齐 RTT 传递、时间驱动的升码率、丢包观察及网络/路由重置。
- 区分实测接收速率和目标码率，保护低码率减法、100% 丢包及大码率计算边界。
- 允许发送端重新保护 NACK 缓存包，验证接收端仍拒绝迟到原包和重复重传。
- 补充 15 项带宽算法测试及弱网接入说明；当前闭环到达控制输出接口。

### 2026-10-10：网络边界、TLS 传输与参与者断线恢复

- 收紧网络对象的调度归属、TCP 启动顺序、缓冲上限及关闭流程，增加服务端 TLS 传输组件。
- TURN UDP 控制端口增加基础 STUN Binding 地址发现。
- 提取参与者身份、信令绑定代数、状态快照和媒体会话数据结构，明确服务与参与者的对象归属。
- 增加恢复窗口、随机恢复凭据和新信令绑定；ICE restart 保留媒体会话、DTLS/SRTP 与 SFU 转发关系。
- 房间示例增加自动重连及重新收集 ICE 候选，完成直接接入/强制 TURN 的双浏览器恢复验证。

### Version 0.2.0 – 2025-12-19

#### Highlights
This release refactors RTSP over TCP handling by separating RTP/RTCP interleaved
media packets from RTSP control message parsing, significantly improving
stability and correctness.

#### Improvements
- Demultiplex RTP/RTCP interleaved packets before RTSP parsing
- Correctly handle TCP sticky packets and fragmented interleaved frames
- Avoid treating empty buffers and partial packets as connection errors

#### Bug Fixes
- Fix RTSP parser being disrupted by interleaved RTP packets
- Fix incorrect connection termination on empty read buffer

#### Design Changes
- Introduce a clear separation between media data plane and RTSP control plane
- RTP/RTCP packets are now consumed at the connection read level

### Version 0.3.0 – 2025-12-30
#### Highlights

This release completes the RTSP-over-TCP interleaved media pipeline by
introducing explicit channel-to-track binding and a codec-aware RTP track
factory, enabling correct RTP/RTCP demultiplexing and per-track packet handling.

#### Improvements

- Introduce RtpInterleaved channel map to dispatch interleaved RTP/RTCP packets
by negotiated TCP channel

- Bind RTP/RTCP interleaved channels to tracks during SETUP negotiation

- Add createTrack() factory to construct codec-specific RTP tracks

- Parse a=control:streamid=N safely via a dedicated ParseStreamId() helper

- Improve RTP debug visibility with optional per-packet header logging

- Generate compile_commands.json via CMake for accurate IDE tooling support

#### Bug Fixes

- Fix incorrect use of track index as interleaved channel ID

- Fix RTP/RTCP packets being routed without validated SETUP binding

- Fix potential session/track mismatch during multi-track SETUP sequences

- Fix undefined reference caused by missing function definitions at link time

#### Design Changes

- Clearly separate RTSP control plane (request/response parsing)
from media data plane (RTP/RTCP packet dispatch)

- RtpTrack lifetime is now managed via shared_ptr, while interleaved dispatch
uses weak_ptr to avoid ownership cycles

- TCP interleaved channels are treated as connection-scoped identifiers,
independent of media track indices


### Version 0.2.1 – 2026-01-01
#### Highlights

- This release fixes RTSP RECORD failures caused by missing track registration during SETUP, and introduces an initial - - RTP-over-TCP interleaved processing pipeline that decouples network I/O from media processing.

#### Improvements

- Register SDP tracks into MediaSession::tracks_ during SETUP to ensure RECORD operates on a fully initialized session

- Add MediaSession::BindRtpTrack(trackIdx, track_ptr) to associate trackIdx -> RtpTrack for deterministic lookup during - - RECORD and media processing

- Introduce an initial worker-based RTP input pipeline (PacketPool + WorkerPool) to offload heavy RTP processing from the connection read thread

- Add structured logging for SETUP track matching (control/codec/pt/clock/trackIdx) and for RECORD session/track validation

#### Bug Fixes

- Fix RTSP RECORD returning no response when MediaSession::tracks_ is empty (root cause: SETUP did not register tracks)

- Fix incorrect/misleading logs in RECORD handler (e.g., printing “SETUP request” in RECORD path)

### Version 0.3.0 – 2026-01-02
#### Highlights

- This release introduces a reusable real-time packet delivery framework for RTP/RTCP processing. It adds a fixed-capacity PacketPool for deterministic memory management and a dedicated SPSC RtpRingBuffer (with RTCP priority) to decouple I/O from media processing, improving stability under load.

#### Improvements

- Add PacketPool (object/memory pool) to provide fixed-capacity packet buffers and avoid frequent heap allocations in the RTP data path

- Add RtpRingBuffer built on SPSC rings, supporting separate RTP/RTCP queues with RTCP-first dequeue policy

- Refactor interleaved RTP ingestion to a “copy + enqueue” model, simplifying the read thread responsibilities and reducing lifetime hazards

- Introduce queue/pool observability hooks (queue depth, dropped/exhausted counters) to facilitate load testing and tuning

#### Bug Fixes

- Fix potential lifetime issues when dispatching interleaved RTP/RTCP across threads by avoiding raw pointer ownership leaks and centralizing buffer release

- Resolve const-correct locking issue in PacketPool by making internal mutex mutable (enables thread-safe const observers such as size()/stats())


### Version 0.3.1 – 2026-01-06
#### refactor(network): fix connection lifetime issues and introduce factory-based initialization

- Introduce two-phase initialization for TcpConnection (construct + Start)
- Move channel registration and event enabling out of constructors
- Add factory method RtspConnection::Create to enforce correct init order
- Replace raw `this` captures in callbacks with weak_ptr to prevent UAF
- Eliminate bad_weak_ptr caused by shared_from_this in constructors
- Unify RTSP connection creation through server OnConnect factory path
- Improve disconnect handling to avoid delayed callback accessing destroyed server


### Version 0.3.2 – 2026-01-07


#### Architecture Changes

- Introduce a generic ShardedWorkerPool with key-based sharding to guarantee
per-key ordering while allowing parallel execution across workers.

- Add a pluggable IJobHandler interface, enabling different subsystems
(RTP, RTCP, media processing, future non-RTSP tasks) to reuse the same worker pool.

- Refactor RTP processing into a clear producer → dispatcher → consumer model:

- IO thread: framing and minimal copy only

- Worker threads: RTP/RTCP consumption and track-level processing

- Decouple RTP consumption logic from RTSP, making the worker service reusable
by other modules.

#### RTP / RTSP Improvements

- Implement a clean RTSP over TCP interleaved handling model:

- Strict $ <channel> <length> <payload> framing

- Robust handling of sticky packets and fragmented frames

- Add explicit channel → track binding established during RTSP SETUP.

- Ensure RTP/RTCP packets are never parsed by the RTSP control parser.

- Move all heavy RTP processing out of IO threads to worker threads.

#### Worker & Scheduling

- Add queue depth limits and drop policies (DropHead / DropTail) to protect
the system under load.

- Guarantee in-order processing for the same track_id via consistent sharding.

- Provide safe shutdown semantics with optional queue draining.

- Improve statistics collection for enqueue, dequeue, drops, and max queue depth.

#### Memory & Stability

- Centralize packet lifetime management via PacketPool.

- Ensure all dropped or unbound jobs properly release packet memory.

- Eliminate potential memory leaks when queues overflow or tracks are unbound.

- Improve defensive checks for invalid channels and oversized RTP packets.

### Version 0.3.3 – 2026-01-11
#### fix(workerpool): fix PacketPool leak by restoring job-based release chain

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


### Version 0.3.3 - 2026-01-13
#### feature: implement RTSP TCP interleaved demux and RTP worker dispatch

- Add RTSP interleaved ($) frame parsing in RtspConnection
- Bind interleaved channel to RtpTrack during SETUP (RTP/RTCP)
- Deliver interleaved RTP/RTCP payloads to media WorkerPool
- Use track-based key for per-track serial processing
- Integrate PacketPool for RTP payload buffering

tips:I'm tired today, going to rest and then continue studying BBR and Gerrit
This establishes the complete TCP → channel → track → worker RTP pipeline.


### Version 0.3.4 - 2026-01-14
#### fix: optimize RTP track map locking and reduce log noise

- Replace std::mutex with std::shared_mutex for RtpJobHandler track map
  to improve concurrency under high RTP load (read-heavy scenario)
- Use shared_lock for track lookup and unique_lock for track insert/erase
- Downgrade excessive ERROR logs to WARN for normal track teardown and race cases
- Improve log context to include key and payload length for easier troubleshooting

tips: I’m just figuring out how to complete my own RTP pipeline, mainly by following ZLMediaKit’s design.
This change avoids unnecessary lock contention in RTP worker threads
and prevents log flooding during track lifecycle transitions.

### Version 0.3.5 - 2026-01- 15
#### feat(rtsp/rtp): introduce RTP wire header parsing and packet reorder pipeline

- Add RtpWireHeader to correctly parse RTP wire-format headers (RFC3550)
- Separate RTP wire header from logical RtpHeader to avoid ABI/layout bugs
- Refactor RtpVideoTracker::inputRtp to parse PT/SSRC/SEQ/Timestamp from wire header
- Integrate EnhancedPacketSortor to handle RTP reordering and wrap-around
- Prepare hooks for PT/SSRC locking and NTP-based timestamp conversion
- Lay groundwork for TCP-interleaved and UDP unified RTP input pipeline

tips: I'm just learn how to work jitter buffer

This change fixes incorrect RTP header parsing, prevents random seq/pt/ssrc
misinterpretation, and enables stable real-time packet reordering.

### Version 0.3.6 - 2026-01- 16
#### refactor(rtsp): rework RTP over TCP interleaved dispatch and job handling

- Introduce channel-to-track binding for RTP/RTCP interleaved streams
- Pass weak_ptr<RtpTrack> through WorkJob instead of relying on handler-side maps
- Clarify ownership and lifetime between interleaved parser and worker threads
- Prepare worker path for safe RTP packet processing

Note:
Further investigation is required for packet lifetime and use-after-free issues.

### Version 0.3.7 - 2026 - 01 - 19
#### fix: resolve RTP packet lifetime issues in worker pool

- Fix heap-use-after-free caused by incorrect ownership of RTP packet memory
- Ensure Packet lifetime is managed consistently across worker threads
- Clarify WorkJob payload semantics (Packet* vs raw buffer)
- Avoid double-free by unifying packet release responsibility
- Improve robustness of RtpJobHandler::handle under concurrent execution

### Version 0.3.8 - 2026-01-20
#### rtp: wire RTP parsing into worker → track pipeline
- Add RTP raw packet handling in RtpJobHandler
- Parse and validate RTP headers before track processing
- Construct RtpPacket from raw bytes and hand off to RtpTrack
- Prepare track-level pipeline for ordered RTP processing
- Lay groundwork for jitter buffer and depacketizer integration


### Version 0.3.9 - 2026-01-21
#### refactor: add RTCP packet type enums and feedback definitions
- Define RTCP packet type enums (SR/RR/SDES/BYE/APP/RTPFB/PSFB/XR)
- Add SDES item type definitions per RFC3550
- Add RTPFB/PSFB feedback type enums (NACK/PLI/FIR/REMB, etc.)
- Use strongly-typed enum class with protocol-aligned values
- Prepare groundwork for RTCP parsing and statistics handling


### Version 0.4.0 - 2026-01-23
#### refactor: rework RTP job payload model and fix abort caused by invalid length'
- Replace void* payload with std::variant
- Use shared_ptr<Packet> for safe cross-thread ownership
- Remove unsafe static_cast paths
- Fix len=0 propagation to inputRtp()

### Version 0.4.1 - 2026-01-27
#### feat(rtp): implement RTP parsing, jitter buffering and H264 depacketization

- Improve RTP header parsing and validation
- Integrate jitter buffer for packet reordering
- Implement ordered RTP callback
- Support H264 Single / STAP-A / FU-A depacketization
- Output Annex-B formatted frames

### DEBUG - 2026-01-28
#### debug: verify RTP pipeline end-to-end and locate PacketPool exhaustion path

- Verified full RTP flow from worker dispatch to RtpVideoTracker::onRtpSorted
- Confirmed callback and sorting stages are correctly triggered
- Identified potential lifetime and caching issues causing PacketPool exhaustion

### Docs - 2026-01-31
#### docs: add RTCP XR (RFC 3611) protocol and SDP signaling notes
- Document RTCP XR common header (PT=207) and packet structure
- Describe the XR report block model and usage scenarios
- Clarify sequence-number–based reporting for Duplicate RLE and Packet Receipt Times blocks
- Explain RTT measurement using Receiver Reference Time (BT=4) and DLRR (BT=5)
- Add SDP a=rtcp-xr signaling rules, distinguishing unilateral and collaborative parameters
- Clarify Offer/Answer behavior and bandwidth considerations for XR usage

This change only updates protocol documentation and design notes, without affecting
existing RTP/RTCP data path logic.

### ROOM:20260204
#### feat: introduce ClientSession for per-client send handling

- Isolate per-client RTP send queue and state into ClientSession
- Prepare MediaSession for SFU-style multi-subscriber forwarding

### RoomVersion:0.0.1: 2026-02-05
#### feat: add basic SFU room

- Introduce Room module to manage participants and provide basic conference routing.
- Add participant join/leave management and broadcast forwarding logic.
- Each incoming RTP packet is forwarded to all other participants (N-1 fanout).
- Prepare foundation for future subscribe-based routing / simulcast / SVC.

### 2026-02-06
#### feat(rtsp/rtp): add RTP packet sorting logs and improve debug tracing
- Add detailed debug logs for RTP sorting pipeline (EnhancedPacketSortor)
- Print seq/next_seq/buffer state to verify jitter-buffer reorder behavior
- Add gdb breakpoint tracing points for emit() / inputRtp() call path
- Improve packet dump helper to validate RTP header correctness
- Facilitate troubleshooting for RTP packet payload/ts/seq parsing issues

### Version 0.4.2 - 2026-02-08
#### feat(core): add signal-based subscription framework for stream dispatch
- Add ISubscription / ISignal interfaces for callback subscription model
- Implement SignalCOW with copy-on-write snapshots; emit does not hold the subscription writer mutex
- Provide subscribe/cancel mechanism to manage listener lifecycle
- Introduce SourceBase<T> abstraction to expose publish/subscribe pattern for stream modules
- Prepare foundation for RTP/frame fan-out and modular pipeline extension

### Version 0.4.3 -  2026-02-09
#### Add Depacketizer module and start H264 RTP frame reassembly

- Introduced a new Depacketizer module and defined a unified interface (input() / hasFrame() / popFrame()) for frame-level reconstruction based on sorted RTP packets.
- Added initial H264Depacketizer class skeleton, preparing the architecture for future codec extensions (H265/VP8, etc.).
- Completed RTP packet metadata filling after parsing, including ts/marker/version/padding/extension/cc/hdr_len/payload_off/payload_len, ensuring downstream depacketization can rely on correct header/payload boundaries.
- Improved RTP header parsing to correctly handle CSRC and header extensions, with additional validation for padding scenarios.
- Added debug logs for RtpSorted output to verify sequence continuity and payload size variations, confirming correct behavior before implementing FU-A/STAP-A reassembly logic.

### Version 0.4.4 - 2026-02-14
#### Introduce RTCP module architecture and prepare RTP/RTCP interleaved processing

- Added initial RTCP module framework following a WebRTC-style interface design, including IRtcpReceiver, IRtcpSender, and IRtcpObserver for clean protocol/business separation.
- Implemented RtcpReceiverImpl skeleton with core entry points (OnRtcpPacket, SetObserver, SetLocalSsrc, SetRemoteSsrc) to prepare for compound RTCP parsing.
- Confirmed RTSP interleaved transport behavior where RTP and RTCP share the same TCP socket, and identified channel-based demux logic (interleaved=RTP-RTCP mapping).
- Prepared media pipeline integration points for routing interleaved RTCP payloads into the RTCP receiver, enabling future support for RR/NACK/PLI feedback handling.
- Reviewed RtspConnection initialization flow and verified worker pool (media) and packet pool integration to support upcoming RTCP parsing and retransmission work.


### Version 0.4.5- 2026-02-15
#### Title: Add RTCP receiver integration and fix build/link issues in RTP track
- Introduced RTCP handling interface (inputRtcp) in RtpTrack and implemented RTCP callback mechanism via IRtcpObserver.
- Integrated RtcpReceiverImpl into RtpVideoTracker to support parsing RTCP packets (RR/NACK/PLI events reserved).
- Fixed namespace and constructor signature mismatch for RtcpReceiverImpl (rtcpx::IRtcpObserver*) to resolve undefined reference issues.
- Updated CMake build linkage to ensure RTCP implementation is correctly compiled and linked for unit tests.
Improved RTSP/RTP module structure preparing for future RTCP feedback processing and keyframe request support.

### Version 0.4.6 - 2026-02-16
#### Refactor TCP stack by introducing a Session/Observer based architecture.
- Added generic ICodec<Msg> interface to support protocol-level decoding/encoding (SIP/RTMP/RTSP, etc.).
- Implemented ObserverList to broadcast decoded messages to multiple business modules via ISessionObserver.
- Introduced IConnectionObserver and improved connection-to-session callback flow for byte-level events.
- Updated TcpConnection to expose bytes callbacks so TcpSession can take ownership of protocol parsing logic.
- Prepared the framework for multi-protocol session management with clean transport/protocol separation.

tips: Today is Chinese New Year. I stayed in my rented apartment and spent the day coding.


### Commit Message (2026-02-17)
Refactored multi-protocol TCP session architecture by introducing ProtocolDetector and ProtocolDetectorSession, enabling dynamic protocol detection and seamless promotion to protocol-specific sessions (SIP/RTSP), while fixing include dependency and compilation issues in SIP parser integration.

### Version 0.4.7 - 2026-02-18
#### Refactored protocol detection and RTSP parser integration by introducing RtspProtocolParser based on ProtocolParser
- Added RtspProtocolParser implementation based on ProtocolParser to support RTSP protocol detection (including $ interleaved framing).
- Refactored parser declarations/definitions and fixed ParseResult scope + missing return issues to resolve compilation errors.
- Updated build integration and linkage to eliminate duplicate Parse declarations and vtable undefined reference errors.

### Version 0.4.8 - 2026-2-20
#### Refactor: Introduce protocol factory and session promotion mechanism
- Added ISessionFactory abstraction and integrated factory injection into TcpServer, enabling protocol-level session creation without coupling transport layer to specific protocol implementations.
- Implemented dynamic session promotion in ProtocolDetectorSession: upon successful protocol detection, create concrete session (e.g., RTSP) via factory and replace current session mapping.
- Optimized detection flow to ensure newly promoted session immediately processes existing buffer data, preventing first-packet loss during protocol switch.
- Decoupled RtspSession from RtspServer to reduce strong dependencies and improve modularity of protocol layer.

### Version 0.4.9 - 2026-2-22
#### feat(network): introduce UDP server abstraction integrated with EventLoop
- Added UdpSocket encapsulation for non-blocking UDP operations (create/bind/recvfrom/sendto).
- Implemented UdpServer with Channel-based integration into existing Reactor (EventLoop + EpollTaskScheduler).
- Introduced IUdpHandler interface for decoupled datagram processing.
- Enabled TCP and UDP servers to coexist under the same EventLoop.
- Prepared foundation for future RTP/RTCP/ICE integration.

### Version 0.5.0 - 2026-2-23
#### feat(media): introduce UDP transport skeleton and session demux layer
- Add MediaEngine as transport entry container to manage UdpServer lifecycle
- Implement UdpMuxHandler to demultiplex STUN / RTP / RTCP / DTLS packets
- Introduce UdpSession abstraction to encapsulate per-peer state
- Define initial session lookup/create mechanism (src-based mapping, preparatory for ICE-lite integration)
- Refactor ownership model: UdpServer uses unique/shared ownership at engine level; sessions hold non-owning reference
- Prepare groundwork for future ICE-lite + RTP integration

### Version 0.5.1 - 2026-2-24
#### udp: add peer-based session routing and protocol demux
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

### 2026-07-26 — RTP 分轨绑定与 Tracker 内存管理

#### 本次完成

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

#### 当前 RTP 上行链路

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

#### 当前弱网与 RTCP 状态

- RTP 有界排序、基本丢包发现和发送端 RTP 重传缓存已经具备。
- RTCP SR/RR、NACK、PLI/FIR、Transport-CC 和 BWE 解析或算法组件已经存在。
- 接收端 NACK 生成、`RtcpDispatcher` 注册、音频 RTCP、PLI/FIR 转发及 BWE 控制动作尚未接入真实媒体链路。
- 下一阶段将优先完成：
  1. RTP 缺包检测 → RTCP NACK → RTP Cache 重传闭环。
  2. SR/RR、RTT、Jitter 与丢包率统计。
  3. PLI/FIR 关键帧恢复。
  4. Transport-CC、BWE 与弱网码率控制。

#### 后续待完善

- 修复 RTP 网络入口超过 1500 字节时可能发生的静默截断。
- 减少 `WorkJob → Packet → Tracker Pool` 的中间内存复制。
- 将 `EncodedFrame` 接入录制、播放、转码或其他业务消费 Buffer。
- 接通原始 RTP SFU 订阅转发链路。

### 2026-07-26 — RTP Track Binding and Tracker Memory Management

#### Completed

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

#### Current RTP Ingress Pipeline

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

#### Current Weak-Network and RTCP Status

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

#### Remaining Work

- Fix possible silent truncation of RTP packets larger than 1500 bytes at the
  network ingress boundary.
- Reduce intermediate memory copies across
  `WorkJob → Packet → Tracker Pool`.
- Connect `EncodedFrame` output to recording, playback, transcoding, or another
  application-level consumer buffer.
- Connect the raw RTP SFU subscription and forwarding pipeline.

### 2026-08-08 — 收紧媒体传输接口并统一 RTSP RTP 接收链路

#### 本次完成

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

#### 当前接收链路

```text
TcpConnection
  → RtspSession（RTSP / '$' 分帧）
  → RtspInterleavedTransport
  → ReceivedMediaPacket
  → MediaEndpointIngress
  → media worker
  → SfuEndpoint / RTP Track
```

#### 后续工作

- 由信令层建立 `UdpSession → SfuEndpoint` 的明确绑定后，将 UDP RTP/RTCP 接入同一个 `MediaEndpointIngress`。
- 减少 `MediaEndpoint::OnRtp()` 到 Tracker Packet Pool 之间的剩余内存复制。
- 接通 Room/SFU 的订阅发送链路，使 `RtpSenderTrack` 通过具体 Transport 完成下行转发。

### 2026-08-15 — 接入 RTCP 接收统计与弱网质量评估链路

#### 本次完成

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

#### 当前计算链路

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

#### 后续工作

- 增加独立的质量评估周期，避免完全依赖上游 SR 的发送周期。
- 周期构造并发送 RTCP RR，复用弱网评估已经生成的 report block，避免重复推进统计区间基线。
- 将目标码率、Pacer 和 FEC 建议接入实际媒体发送或编码控制接口。
- 完善下行 `RtpSenderTrack` 的 RR/TWCC、RTT 和带宽估计控制链路。
- 增加 Endpoint/Session 级音视频质量聚合策略。

### 2026-09-14 — 完善录像生命周期并接通发送侧 GCC 反馈链路

#### 本次完成

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
- 新增 8 个发送侧链路测试及[接入文档](#弱网与带宽控制)。

#### 验证情况

- 发送侧与原有弱网测试共 16 项，独立构建运行通过。
- 完整服务及录像改动尚未完成本轮联调。

#### 后续工作

- 当前 GCC 链路接至控制输出接口，继续接入实际 Pacer 发包和编码器调码率。
- 实现 TWCC 超时后的 RR 回退策略。

### 2026-09-23 — 重构 SFU 端点，支持多轨发布与订阅

#### 本次完成

- 将 SDP 媒体描述统一转换为现有 RTP Track 参数，现由 `Rtsp/Rtp` 中的 `RtpStreamContext` 和 `SdpTrackBinding` 承接。
- 支持单个 `SfuEndpoint` 管理多条发布轨道，按 MID、SSRC 和轨道提示分流。
- 将接收 Track 归属发布端点、发送 Track 归属订阅端点，通过工作线程投递完成跨端点转发。
- 接通房间发布、订阅、取消订阅、取消发布和参与者退出的媒体生命周期。
- 支持下游 RTP 参数及 MID/TWCC 改写，接通 NACK 缓存重传和 PLI 反馈。
- 调整 RTSP 多轨共享端点，完善 SETUP 失败回滚和 TEARDOWN 清理。
- 新增 `WebRtcMediaTransport`，适配 WebRTC 会话与统一媒体收发接口。
- 完善订阅失效、排队旧包丢弃及端点停止后的资源清理。
- 补充多轨转发、房间订阅、SDP 绑定、WebRTC 传输适配和 RTSP 多轨集成测试。

#### 验证情况

- 主程序编译通过，新增 10 项测试全部通过，RTSP、SDP 和录制相关回归通过。
- 完整回归仍有两项已知失败：WebRTC STUN 提名、超大 RTP 包计数。

#### 当前限制

- WebRTC 应用层信令组装及实际 DTLS/SRTP 后端仍待接入，尚未完成真实浏览器链路验证。
- 暂不支持 RTX 和 simulcast 层选择；每条订阅固定转发一个源编码的 SSRC。
- 房间接入仍需应用层绑定参与者端点，并根据下游协商结果配置订阅参数。

### 2026-09-24 — 引入公共状态机并接入 ICE 与 RTSP 会话

#### 本次完成

- 新增 `StateMachine` 和 `StateController`，支持强类型转换表、条件判断、动作回调和绑定业务上下文。
- 明确非法事件、重复规则、递归派发和异常处理语义。
- 将 ICE 生命周期改为显式状态，接通存活超时与 WebRTC 会话关闭，阻止关闭后的连接被迟到包激活。
- 修复 STUN MESSAGE-INTEGRITY 计算范围错误，防止认证失败的请求污染远端凭据及未认证属性触发提名。
- 将 RTSP 推流流程接入状态机，保留多轨 SETUP 和失败回滚，成功处理请求后才提交状态转换。
- 校验 RECORD/TEARDOWN 的会话归属，支持重复 RECORD、TEARDOWN 后复用 TCP 连接及断线清理。
- 未实现的 DESCRIBE、PLAY、PAUSE 明确返回 501。
- 补充公共状态机、ICE 和 RTSP 生命周期测试；相关实现见 [IceAgent](protocol/ice/IceAgent.cpp) 及 [RtspSession](Rtsp/RtspSession.cpp)。

#### 验证情况

- 主程序编译通过，公共状态机、ICE、WebRTC 会话、传输及媒体适配相关测试通过。
- RTSP TCP/UDP、多轨 SETUP、失败回滚、错误请求顺序、会话重建和断线清理测试通过。
- 上一条记录中的 STUN 提名失败已修复；超大 RTP 包计数问题不在本次修改范围内。

#### 当前限制

- 应用需在会话事件循环周期调用 `WebRtcSession::Tick`，确保无流量时也能及时检测 ICE 超时。
- ICE 建立与存活超时仍共用配置；会话级 ICE restart、完整主动检查及真实浏览器互通仍待完善。
- RTMP、AI 和公共服务生命周期尚未迁移至状态机组件。

SFU 信令模块划分、线程与双向通道、失败回滚和测试记录见 [重构说明](#媒体层结构与命名)。

### 2026-10-27 — 统一媒体参数分层、RTSP 鉴权与房间轨道事件

#### 本次完成

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

#### 当前调用链与线程归属

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

现有目录职责见[RTP 参数与 SDP 适配](#媒体层结构与命名)和[房间信令与轨道事件](#媒体层结构与命名)。

#### 验证情况

- `Packetia`、`PacketiaControl`、`PacketiaSfu` 构建通过；链接器仍有重复库警告。
- 媒体隔离阶段：`server_app`、`media_parameters`、`test_rtp_input`、`test_udp_binding`、`rtsp_auth`、`webrtc_service` 共 6 组 CTest 通过。
- 录像独立测试 3/3 通过，转码独立测试 3/3 通过；这两项为媒体隔离阶段结果，Bus 接入后未重复运行。
- Bus 接入后：`server_app`、`media_parameters`、`test_rtp_input`、`webrtc_service`、`signal_channel` 共 5 组 CTest 通过。
- 新增 Bus 句柄析构、退订快照过滤、回调异常隔离、弱引用及捕获对象析构测试；新增房间发布/移除、同名重发、离房和重复关闭测试。
- 真实 WebSocket 集成测试覆盖发布主动通知、跨房间隔离、重连快照补发和恢复超时后的移除通知；补充通知合并及版本不变时不重复发送的断言后，`webrtc_service` 再次通过。
- 示例页面通过 `node --check`，`git diff --check` 通过；本次未重新运行浏览器媒体互通、公网、容量或完整回归测试。

#### 当前限制与后续工作

- WebRTC 房间轨道仍由 Room/Participant 管理，尚未全部统一到 `MediaStreamRegistry`；业务字符串轨道 ID 与 core 流内数值 ID 仍是不同作用域。
- 完整 WebRTC SDP 保留在连接会话的协商层；`RtpStreamContext` 是参数子集，不存完整 SDP。RTSP 当前保留 RTP 快照和 control 映射，不长期保存完整 SDP 描述。
- `tracks_changed` 是最新完整列表，多次变化可能合并；客户端必须与请求响应分流。通知不代表已开始收到媒体，也不会自动创建订阅或触发动态重协商。
- 轨道级 Open/Closing/Closed、订阅意图与重试、动态权限、静音转发控制、通用 MediaStream/RTSP 流状态事件尚未接入。
- `MediaStatsCore` 的 RTP/RTCP/帧事件处理仍为空实现，接口隔离不等于已经接通实时统计。
- 本次未新增 RTMP/SRT 接入或跨协议输出，也未引入 RTX、simulcast 层选择或新的拥塞控制算法。

### 2026-10-28 — 将 RTP 参数与 SDP 适配移回协议目录

- 将 `RtpStreamContext`、`RtpTrackBinding`、`SdpTrackBinding` 共 5 个文件从 `media/rtp` 迁入现有 `Rtsp/Rtp`。
- 在 `Rtsp/Rtp/CMakeLists.txt` 定义 `packetia_rtp_binding`、`packetia_sdp_rtp_adapter`，替代前一日的 `packetia_media_rtp`、`packetia_sdp_media_adapter`。
- 同步调整头文件搜索目录、媒体端点及参数测试的目标依赖；保留短名称 include 和原有调用接口。
- 通用轨道仍位于 `media/track`，RTP/SDP 转换供 RTSP 与 WebRTC 复用；共享传输抽象继续位于 `media/transport`。
- 仅整理目录和构建归属，不修改协商参数选择、轨道 ID 或媒体处理算法。
- 验证：`Packetia`、`PacketiaControl`、`PacketiaSfu` 构建通过；`media_parameters`、`test_rtp_input`、`test_udp_binding`、`webrtc_service` 共 4 组 CTest 通过。纯 RTP 选择实现仅使用协议目录及通用媒体头文件即可独立语法编译。

### 2026-10-28 — 留在会议中停止媒体并重新发起

- 新增 `unpublish` / `unsubscribe` 信令，停止整个当前发布或订阅媒体会话，保留参与者、房间、信令连接和恢复凭据；可在同一次入房后重新发布或订阅。
- 请求必须携带媒体 Answer 中的正整数 `session_id`，并通过对应权限、当前编号和媒体角色检查。重复停止在没有当前媒体时返回 `changed:false`；已有新媒体时拒绝旧编号，避免误清理。
- 复用 `RemoveMediaSession`，在服务 owner 执行器回收轨道/路由、Participant 媒体绑定、RTC、传输、UDP mux 登记及 Endpoint。停止发布通过现有 Bus 推送 `tracks_changed`；响应沿原通道串行发送。
- 示例页面增加 Stop publishing / Stop watching，确认后关闭 PeerConnection，发布端释放摄像头与麦克风。补充无媒体恢复及旧连接候选/统计回调的过滤。
- 真实 WebSocket 集成测试覆盖权限与参数拒绝、角色检查、重复停止、旧编号保护、资源及下游路由回收、移除/重发通知、重新订阅和停止媒体后的身份恢复。
- 验证：`Packetia`、`PacketiaControl`、`PacketiaSfu` 构建通过；`webrtc_service`、`signal_channel` 两组 CTest 通过；页面及浏览器回归脚本的 `node --check`、`git diff --check` 通过。
- 浏览器回归脚本已补充停止/重建流程，但未运行媒体互通：当前源码缺少可选的 `server/examples/WebRtcRoomDemo.cpp`，无法重建对应演示服务。未验证公网和容量。
- 当前仍为每连接一个媒体会话，操作作用于整会话；源轨道重发后，观看端需显式停止旧订阅并重新订阅，不自动恢复路由。协议详情见[房间信令说明](#媒体层结构与命名)。

### 2026-10-29 — WebRTC 房间固定 owner

- 根据房间 ID 固定分配执行器，同一房间的成员、协商、发布、订阅和清理串行处理；不同 owner 的房间可独立推进。
- 网络执行器保留 WebSocket 和一个共享 UDP 端口；信令、通知和数据报通过异步通道跨 owner 转发。
- 异步请求完成前保留预算并暂停下一条请求，保证同连接离房后跨 owner 入房的顺序；成员和媒体会话额度仍按全服务限制。
- 增加 UDP 队列与旧路由代次过滤；执行器拒收断连清理时由 Tick 重试，停服按接入、房间、网络顺序回收。
- 原 `Packetia` 入口配置 `server.io_threads: 3` 可使用 1 个网络执行器和 2 个房间 owner；独立 `PacketiaSfu` 入口当前仍为单 I/O 线程。
- `Packetia`、`PacketiaControl`、`PacketiaSfu` 构建通过；`server_app`、`test_datagram_transport`、`webrtc_service`、`signal_channel` 共 4 组 CTest 全部通过，`git diff --check` 通过。
- 未重新验证浏览器媒体、公网或容量；线程模型、实际文件和调用链见[房间 owner 说明](#webrtc-房间-owner)。

### 2026-10-29 — 分离通用队列与 UDP 发送职责

- 将通用排队、预算、门控和异步完成机制提取到 `utils/BoundedChannel.h`；`SignalChannel` 保留信令接口与首响通知顺序。
- 新增 `network/transport/UdpSendQueue`，队列保存目标地址、独立缓冲区与传输弱引用；网络执行器负责实际发送。
- `WebRtcService::Start` 只组装发送队列和 UDP mux；UDP 入站任务也改用公共队列，移除对信令队列的依赖。
- UDP 默认 4 MiB / 4096 条限制集中定义，原有预算、关闭检查及排队顺序保持；新增真实 UDP 缓冲区、背压和取消回归。
- 验证：三个运行入口与相关测试构建成功，`server_app`、`test_datagram_transport`、`webrtc_service`、`signal_channel` 共 4 组 CTest 通过；公共队列独立语法编译及 `git diff --check` 通过。

### 2026-11-04 — 下行订阅带宽策略、指标平滑与接收评估器生命周期

#### 本次完成

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

实现与接口示例见[弱网与带宽控制](#弱网与带宽控制)，主要代码位于
`media/quality/DownlinkBandwidthAllocator.*`、`media/quality/NetworkStatsCollector.*`、
`media/quality/ReceiveQualityMonitor.*`、`media/endpoint/` 和 `Rtsp/Rtp/VideoResumeBuffer.*`。

#### 验证情况

- `Packetia` 及相关测试目标构建通过。
- 新增订阅预算、视频暂停恢复、指标平滑与采样测试；另新增 4 项接收评估器生命周期测试，覆盖重复注册、无样本初始状态、创建失败、SR 先到、删除及停止清理。
- 最终回归中，`test_weak_network_manual`、`test_rtp_input`、`test_udp_binding`、`media_core`、`test_webrtc_crypto`、`webrtc_service` 共 6 组 CTest 全部通过。

#### 当前边界

- 自动分配只使用初始化完成且最近 1500 ms 内有有效 TWCC 的估计；启动阶段、反馈过期和 RR-only 连接保持原分配。
- 当前支持整条视频订阅暂停与恢复，尚未实现 simulcast/SVC 层选择；保留音频不代表预算不足时仍能满足全部音频需求。
- 接收侧质量评估及 RR 发送仍由 SR 触发，公共 Tick 尚未接入周期质量评估；本次提前的是评估器创建时机。
- 平滑质量指标用于观察趋势，质量降级、NACK/PLI 和拥塞控制仍读取原始反馈；源码率的平滑结果用于订阅需求估算。
