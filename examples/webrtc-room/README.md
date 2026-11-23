# Packetia 传输实验室

用于学习和验证 SFU 单层转发的简单前端。使用原生 JavaScript，无需前端编译步骤。
同一页面支持摄像头发布、多人订阅或同时收发，复用项目现有房间信令、ICE、DTLS/SRTP 与 TURN。

默认启动方式通过本地开发服务签发令牌，直连一个 `PacketiaRoomDemo`，不经过 `PacketiaControl`
的 `/api/v1/room-admissions`。浏览器质量测试验证单节点媒体与反馈链路，尚未覆盖集群房间调度
或单房间跨节点分发；集群现状与目标见[项目 README](../../README.md#集群现状与演进方向)。

已有 `packetia-cluster:local` 公网部署时，使用 `npm run start:cluster` 连接真正的 Control/SFU。
该入口通过 SSH 使用真实账号 API 和入房许可，不启动本机媒体后端；
服务器注册开关、隧道、UDP 端口与操作步骤见[集群公网浏览器测试](../../deploy/cluster/SSH-BROWSER-TEST.md)。

希望直接在浏览器访问公网 IP 时，使用[公网 HTTPS 测试入口](../../deploy/cluster/PUBLIC-BROWSER-TEST.md)：
页面运行在服务器，复用已有后端镜像，支持测试密码登录和 WSS；电脑信任测试证书后即可推流和观看。

## 启动

需要 Node.js 20+（推荐 22/24）、C++ 构建依赖，以及开启 WebSocket/libSRTP 的构建目录。
完整依赖配置见[项目参考文档](../../docs/reference.md#浏览器会议与-turn-示例)。

在仓库根目录构建 C++ 示例，然后启动前端：

```sh
cmake --build build/webrtc-integration --target PacketiaRoomDemo -j4
cd examples/webrtc-room
npm ci
npm start
```

打开 `http://127.0.0.1:18000`：

1. 第一个窗口使用房间 `demo`，选择 Publisher，点击 Join。
2. 选择分辨率、帧率和编码码率上限，点击 Publish camera，允许摄像头和麦克风。
3. 第二个窗口选择同一房间，角色 Viewer，Join 后选择发布者，再点击 Watch。
4. 推流期间修改码率上限，点击“应用码率上限”，对照两端的实际码率和接收帧率。
5. 点击“导出 JSON”保存本窗口统计；想对照发送与接收，需要分别导出两个窗口。

分辨率和帧率在开始推流时请求，实际值以摄像头采集和逐轨统计为准。
修改分辨率/帧率需要停止并重新发布；重新发布后，接收端需停止订阅再 Watch。
推流端仍使用现有 H264/Opus 协商，没有添加 VP9/AV1、SVC 或服务器转码。
当前下行控制器默认上限为 2.5 Mbps，页面选择 8 Mbps 并不会提高服务器约束。

默认端口：HTTP 18000、WebSocket 18080、SFU UDP 19000、TURN UDP 13478。
端口占用时，可分别设置 `PACKETIA_DEMO_HTTP_PORT`、`PACKETIA_DEMO_WS_PORT`、
`PACKETIA_DEMO_MEDIA_PORT` 和 `PACKETIA_DEMO_TURN_PORT`。
其他构建产物通过 `PACKETIA_ROOM_DEMO_BINARY` 指定。
启动器默认仅监听本机并生成临时开发凭证；通过公网域名提供页面时，需要独立配置 HTTPS/WSS、鉴权及媒体可达地址。

只有公网 IPv4、尚无域名时，可使用 [SSH 转发与 Docker 媒体测试](../../deploy/browser-test/README.md)。
页面和信令经 SSH 到电脑的回环地址，媒体直接走服务器 UDP 19000。
该模式通过 `PACKETIA_DEMO_PUBLIC_IP` 指定媒体候选地址、`PACKETIA_DEMO_LISTEN_IP` 指定进程监听地址；
两者默认仍为 `127.0.0.1`。远程模式不提供本机 TURN，页面相应禁用强制 TURN 选项。

## 服务端诊断日志

启动器默认开启 `PACKETIA_QOS_LOG=1`，使用 JSON 格式，将日志写入
`artifacts/server-qos.jsonl`，同时输出控制台。文件达到 16 MiB 时轮转，保留 3 份备份。
`PACKETIA_LOG_MODE`、`PACKETIA_LOG_FORMAT`、`PACKETIA_LOG_FILE` 可覆盖默认设置。
设置 `PACKETIA_QOS_LOG=0` 可关闭媒体诊断事件；变更环境变量后需重启启动器。

```sh
npm run qos:summary
```

该命令汇总当前日志为 `artifacts/server-qos-summary.json`。
通过 `PACKETIA_QOS_TRACE_FILE` 指定日志文件，`PACKETIA_QOS_ROOM` 仅汇总一个房间。
按 `pid + node_id + transport_id` 关联记录，其中浏览器 `sessionId` 对应该进程的 `transport_id`。
当前只采样下行订阅连接，发布会话仅记录身份映射，因此发布会话样本数为 0 属于预期行为。

需要验证浏览器测试确实采集到服务端发送量、RTCP 反馈和分配日志时：

```sh
PACKETIA_EXPECT_QOS_LOG=1 npm run check:quality
```

验证 TWCC 完整输入链路时，使用更严格的检查：

```sh
PACKETIA_EXPECT_TWCC=1 npm run check:quality
```

它要求初次订阅和重新订阅都协商出非零扩展 ID，并实际收到可匹配发送历史的 TWCC，
产生回归斜率、有效带宽估计和分配预算。该选项包含上述服务端日志检查。

非默认 HTTP 端口仍通过 `PACKETIA_DEMO_URL` 指定。
测试会在 `quality-result.json` 中加入 `server_diagnostics`，并单独保存本房间的
`quality-server-qos.jsonl` 和 `quality-server-summary.json`。
完整字段、算法解释和 TWCC 协商链路见[服务端诊断说明](../../docs/media-quality-diagnostics.md)。

## 怎么读指标

| 指标 | 计算或来源 |
| --- | --- |
| 实际码率 | 单条 RTP 统计的字节增量 × 8000 / RTCStats 时间差（ms） |
| 包速率 | 包数增量 × 1000 / 时间差（ms） |
| 发送 / 接收帧率 | 编码 / 解码帧数增量 × 1000 / 时间差（ms），每轨独立展示 |
| 接收区间丢包率 | 丢包增量 /（接收包增量 + 丢包增量） |
| 抖动 | 接收端 `jitter` 从秒换算成毫秒 |
| 缓冲等待 | `jitterBufferDelay` 增量 / `jitterBufferEmittedCount` 增量，换算成毫秒 |
| ICE RTT | 订阅连接选中 candidate pair 的 `currentRoundTripTime`，换算成毫秒 |
| 冻结累计 | 浏览器报告的 `freezeCount`，未支持时显示 — |
| 编码限制 | 浏览器的 `qualityLimitationReason`，例如 cpu、bandwidth 或 none |

首个样本、计数器重置、SSRC 变化和缺失指标显示 —，导出为 `null`。
迟到包导致累计丢包数减少时，该区间不生成新的丢包率。
音频和视频分开统计；曲线只汇总视频码率，不累加多条视频的帧率。
采样约一秒一次，曲线显示最近 90 个样本，JSON 最多保留 600 个样本和 100 条事件。
后台标签页可能降低计时器频率，所以速率始终使用实际 RTCStats 时间差。
导出包括实际采集、接受的编码参数和逐轨统计，不包含 JWT、恢复令牌、TURN 密码或 SDP。

这些面板指标来自浏览器，不是服务器内部的 BWE 估计、Pacer 排队或探测状态；ICE RTT 也不是端到端播放时延。
`maxBitrate` 是编码器上限，不保证实际发送达到该值，也不能模拟网络拥塞。

## 自动验证

已部署公网 HTTPS 集群测试入口时，可在电脑上运行真实公网收发检查：

```sh
PACKETIA_PUBLIC_TEST_PASSWORD='服务器生成的测试访问密码' npm run check:public
```

需要先安装本目录依赖和 Chrome，并在电脑上信任测试 CA。CA 文件默认读取仓库根目录的
`ca.crt`，其他位置通过 `PACKETIA_PUBLIC_TEST_CA` 指定；默认服务器为
`https://45.76.105.146:18443`，其他入口通过 `PACKETIA_PUBLIC_TEST_URL` 指定。
开始前将手动测试页面中的 Publisher/Viewer 点击 Leave，释放两个固定测试身份。

该检查启动两个独立 Chrome 进程，使用模拟摄像头和音频，通过实际公网 SFU 收发。
默认每个主要阶段采样 20 次，约一秒一次，覆盖 360p30 的 900 → 300 → 1500 kbps
编码上限调整、订阅 ICE 重启和 720p60 采集请求，并记录实际输出而非将请求值当成结果。
`PACKETIA_PUBLIC_TEST_SECONDS` 可设为 5–120；重启后阶段最多采样 10 次。
编码上限调整不模拟网络限速，测试没有注入网络丢包或延迟，也不测多人并发容量。

每次运行在 `artifacts/public-transport-时间戳/` 下保存 `REPORT.md`、`summary.csv`、
原始 `result.json` 和接收画面截图，不覆盖原有本机报告。密码、入房令牌、Cookie 和完整 SDP
不写入这些产物；错误或中断会保留失败阶段数据。服务器 BWE/TWCC/Pacer 状态仍需配合服务端日志。

集群控制服务故障下的媒体连续性检查见
[控制服务与媒体服务的职责边界](../../docs/control-media-boundaries.md#可复现验证)。
它使用实际业务账号和入房许可，自动启动临时 Control/SFU，验证控制服务停机、
租约过期与恢复期间的音视频转发，无需提前运行本示例开发服务器。

在此目录执行：

```sh
npm test
npm run check:algorithms
```

- `npm test`：统计口径、时钟/计数器变化、丢包修正、音视频区分与参数校验。
- `check:algorithms`：构建并运行现有 C++ 的四组测试，包括 WebRTC 趋势对照、Pacer、
  带宽分配、拥塞反馈、恢复门槛、Padding/TWCC 探测与发送链路。
  通过 `PACKETIA_BUILD_DIR` 指定其他已配置且开启测试的构建目录；日志强制使用 console。

保持 `npm start` 运行，另开终端：

```sh
cd examples/webrtc-room
npm run check:quality
npm run check
```

浏览器检查需要安装 Google Chrome；质量检查显式将模拟摄像头设置为 60 fps。其他可执行路径使用 `PACKETIA_CHROME_EXECUTABLE`。
HTTP 端口被修改时，例如使用 18100：

```sh
PACKETIA_DEMO_URL=http://127.0.0.1:18100 npm run check:quality
```

- `check:quality`：两个独立浏览器进程使用模拟摄像头/麦克风，验证真实 SFU 转发、逐轨测量、
  900 → 300 → 1500 kbps 编码上限在线调整、ICE restart 后继续解码、720p60 请求及实际输出记录、
  JSON 下载和手机布局。高帧率阶段还要求模拟采集达到 50 fps 以上，且至少一个接收测量窗口达到 45 fps；
  人工使用真实摄像头时，以实际采集值为准。
- `check`：原有三浏览器双向音视频、TURN、信令恢复、独立停止/重建及资源释放检查。

产物在已忽略的 `artifacts/` 目录：

| 文件 | 内容 |
| --- | --- |
| `algorithms-result.json` / `algorithms-junit.xml` / `algorithms.log` | C++ 测试状态与详细输出 |
| `quality-result.json` | 各阶段发送/接收统计及断言结果 |
| `quality-receiver-export.json` | 自动下载的接收端质量报告 |
| `quality-desktop.png` / `quality-mobile.png` | 浏览器页面截图 |
| `duplex-result.json` | 原有三浏览器联调结果 |

C++ 测试验证可重复的算法行为；浏览器测试验证本机真实收发及统计链路。
当前测试没有对媒体 UDP 注入带宽、延迟或丢包，不能据此宣称公网弱网效果、20 Mbps 能力或多人容量达标。
测拥塞降码率、暂停后恢复，需要在实际媒体路径施加网络条件，并结合服务器指标观察。
Chrome 的 HTTP 请求限速不能替代 WebRTC UDP 弱网模拟。

## 本次验证记录（2026-11-18，macOS）

- Node.js 24.22.2、Google Chrome、当前 `build/webrtc-integration` 构建。
- 前端统计及日志汇总测试：11/11 通过；含结构化日志的 C++ CTest：5/5 组通过。
- 双浏览器质量联调及原有三浏览器会议/TURN/恢复检查通过。
- 补齐下行 TWCC 协商后，6/6 组信令测试与 `PACKETIA_EXPECT_TWCC=1` 质量检查通过。
  新的 `quality-server-summary.json` 中两次订阅均为 TWCC 输入，分别记录 320/96 份反馈、
  1653/662 个匹配包，未匹配回执项均为 0，且有回归斜率与有效分配预算。
- 模拟源请求 360p30 时，实际采集 30 fps，接收解码均值约 30 fps。
- 模拟源请求 720p60 时，实际采集 60 fps，六个测量窗口的接收解码均值约 60 fps，
  接收视频码率均值约 349 kbps。
- 这证明了本机较低码率下的高帧率转发，未验证 8/20 Mbps、高并发或公网弱网容量。

本机默认 Homebrew Node 26 缺少 `libsimdjson.33.dylib`，测试改用已安装的 Node 24，
没有修改系统链接或安装依赖。遇到相同问题，可在当前终端使用：

```sh
export PATH="/opt/homebrew/opt/node@24/bin:$PATH"
```

验证期间默认媒体端口已占用，因此使用 HTTP 18100、WS 18180、媒体 UDP 19100、TURN UDP 13578。

## 阅读代码

```text
index.html / style.css   页面与布局
app.js                  房间信令、采集参数、编码上限和媒体连接
quality-stats.js         逐轨计数差分、单位换算、参数校验
quality-panel.js         表格、码率曲线、采样记录和 JSON 导出
dev-server.mjs          本地静态文件、开发 JWT 与 C++ 示例启动
check-quality.mjs       浏览器质量联调
check-algorithms.mjs    现有 C++ 算法测试入口
```
