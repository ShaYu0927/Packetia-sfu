# Packetia SFU

基于 C++17 的音视频服务学习与实验项目，围绕房间、发布订阅和 RTP/RTCP 转发实现 SFU，
并提供录制、转码、混流及 AI 处理组件的接入边界。

项目正在逐步拆清媒体核心、业务控制和处理组件的职责。当前支持单进程运行，以及独立控制服务与多个 SFU 节点；
处理组件默认仍在单进程入口内运行，外部 Agent、Egress 和处理任务 RPC 尚未实现。

- [当前能力与限制](#当前能力与限制)
- [架构与模块](#架构与模块)
- [构建与依赖](#构建与依赖)
- [运行方式](#运行方式)
- [处理组件与构建目标](#处理组件与构建目标)
- [测试](#测试)
- [后续拆分方向](#后续拆分方向)
- [详细文档](#详细文档)

## 当前能力与限制

| 模块 | 已有能力 | 当前边界 |
| --- | --- | --- |
| WebRTC / SFU | WebSocket 信令、H264/Opus 协商、ICE-Lite、DTLS/SRTP、房间发布与订阅 | 示例每个参与者只承担发布或订阅一种媒体角色；动态轨道重协商尚未开放 |
| RTP / RTCP | 接收排序、H264 组帧、NACK、PLI/FIR、TWCC/RR 与发送侧带宽控制 | Pacer、simulcast/SVC 选层和 FEC 尚未接入；其他视频格式的完整 RTP 接入需分别完善 |
| 断线恢复 | 信令恢复、参与者保留、同会话 ICE restart | 同进程内恢复，不包含服务重启或跨节点媒体迁移 |
| TURN / STUN | UDP 中继、长期凭证认证、权限与通道、IPv4/IPv6 | 不支持 TURN TCP/TLS；TLS 传输组件尚未接入 WSS/TURN 监听 |
| 录制 | H264/AAC fMP4 切片、逐流启停、SQLite 索引与查询 | 原始流录制与处理输出的保存需要分别组装 |
| 视频转码 | FFmpeg 软件编解码，支持 H264/H265/VP8/VP9/AV1，一次解码生成多档输出 | 依赖本机 FFmpeg 实现；尚无远程任务 API、派生流 RTP 输出与浏览器播放闭环 |
| 混流 | 输入配置、任务生命周期、有界队列及输出接口 | 实际拼画面、混音和编解码后端需要实现 `IMixBackend` |
| AI | 模型提供者接口、请求编排与帧处理入口 | 默认提供者未配置模型，不会执行实际推理 |
| 控制面 | 账号与会话、房间管理、节点注册与心跳、房间分配、JWT 准入 | 单控制服务，尚无节点故障迁移 |
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

| 目录 | 主要职责 |
| --- | --- |
| `server/` | 应用启动、信令接入、HTTP 接口和进程组装 |
| `service/core/` | 通用服务生命周期 |
| `service/meeting/`、`service/Cluster/` | 会议业务、节点租约和房间调度 |
| `service/processing/` | 处理组件接口集合、工厂类型和默认本地组装 |
| `service/RecordService/` | 录制、文件、切片及索引 |
| `service/AIService/` | 模型接口、AI 编排和帧入口 |
| `service/ConferenceMixService/` | 混流任务和后端接口 |
| `service/TranscodeService/` | 转码任务、队列和运行时 |
| `media/` | 媒体类型、流与轨道、帧路由、端点、质量控制和 codec 适配 |
| `Rtsp/`、`protocol/` | 协议、RTP/RTCP、ICE、WebRTC、TURN 等实现 |
| `network/`、`utils/` | 网络传输、EventLoop、线程与有界队列 |
| `config/`、`monitoring/` | 配置快照与运行指标 |

## 构建与依赖

需要支持 C++17 的编译器、CMake 3.16 以上版本、`pkg-config`、OpenSSL、SQLite 3.24 以上版本，
以及 FFmpeg 的 libavformat、libavcodec、libavutil、libswscale 开发库。
默认启用 libwebsockets；WebRTC 媒体运行需要 libsrtp2，控制面 HTTP 客户端需要 libcurl。

`BUILD_TESTING` 默认开启，需要 GoogleTest。录像集成测试另需 `ffmpeg` 命令行、Python 3 及相应编码器。
项目使用系统 FFmpeg 开发包，无需编译 `third/ffmpeg`。
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

单进程入口提供账号 API 和本地服务；会议创建、成员管理、节点分配和入房 JWT 签发由 `PacketiaControl` 提供。
独立 `PacketiaSfu` 当前专注房间和媒体转发，本地录制、AI、混流、转码组件尚未接入该入口。
接口和权限说明见[会议控制与节点调度](docs/reference.md#会议控制与节点调度)。

浏览器示例前端位于 [examples/webrtc-room](examples/webrtc-room)。对应服务目标 `PacketiaRoomDemo`
仅在 `server/examples/WebRtcRoomDemo.cpp` 存在时生成，当前源码快照缺少该入口。
已有兼容二进制时，可通过 `PACKETIA_ROOM_DEMO_BINARY` 指定；原有运行步骤见
[双浏览器房间与 TURN 示例](docs/reference.md#双浏览器房间与-turn-示例)。

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
ctest --test-dir build/dev --output-on-failure
```

本次模块拆分的相关回归入口：

```sh
ctest --test-dir build/dev --output-on-failure \
  -R '^(server_app|server_launcher|server_config|startup_config|stream_services|ConferenceMixFramework|transcode_domain|video_transcoder|multicodec|webrtc_service|MeetingControlIntegrationTest)$'
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
- [会议混流](service/ConferenceMixService/README.md)：任务、后端契约与输出边界。
- [录像合成示例](service/RecordService/examples/README.md)：离线拼画面与混音实验。
- [解包与弱网测试](Common/Depacketizer/Test/README.md)：H264 接收与恢复实验。
