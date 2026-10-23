# 服务端切网恢复框架

2026-10-23：框架接入核心 C++ 源码，不依赖浏览器 demo。客户端仍负责生成 ICE restart offer，服务端负责验证新路径、保留会话与转发关系，以及更新媒体控制。这里没有实现手机切换网络，也不代表已经完成真实 4G/5G 验收。

## 按调用链阅读

```text
客户端 restart_ice → WebRtcService → WebRtcSession::RestartIce
    → 协商与 UDP mux 路由代次提交成功
    → NetworkPath: Checking → 暂停发送侧控制，作废旧队列任务
    → 新凭据的 STUN 检查与提名 → 保留的 DTLS/SRTP 状态可用
    → NetworkPath: Ready → 重置旧路径估计，等待新反馈
```

无需重新协商的地址变化也有入口：认证后的新地址提名成功，且 UDP mux 注册通过后，Ready 的 `path_revision` 增加，发送侧执行相同的路径重置。重复 STUN 检查/同地址重复提名不产生新的 Ready 事件。

| 核心文件 | 读什么 |
| --- | --- |
| [WebRtcNetworkPath.h](../protocol/WebRtc/WebRtcNetworkPath.h) | 纯状态：New / Checking / Ready / Failed / Closed，ICE 代次、路径修订号和当前地址 |
| [WebRtcSession.h](../protocol/WebRtc/WebRtcSession.h) | `NetworkPath()` 查询及 `onNetworkPathChanged` 观察接口 |
| [WebRtcSession.cpp](../protocol/WebRtc/WebRtcSession.cpp) | `CommitNegotiation`、`HandleStun`、`CompleteDtls`、`NotifyNetworkPath`，真实触发位置 |
| [WebRtcService.cpp](../server/WebRtcService.cpp) | 原有鉴权及 mux 注册；新回调将路径状态转交媒体适配器 |
| [WebRtcMediaTransport.cpp](../media/transport/WebRtcMediaTransport.cpp) | 状态到媒体控制的适配，路径代次检查使旧异步发送失效 |
| [SendSideController.cpp](../media/quality/SendSideController.cpp) | `OnNetworkRouteChanged()` 清历史与估计，保留发送身份 |

建议先从 `WebRtcNetworkPath.h` 读起，再在 `WebRtcSession.cpp` 搜索 `NotifyNetworkPath`，最后看 Service 如何连接两个层次。协议层不依赖 Room、订阅或带宽算法，媒体层不解析 ICE 凭据。

## 状态与线程约定

- 初始启动和每次已提交的 ICE restart 增加 `ice_generation`；失败的协商准备不推进代次，也不暂停原路径。
- Ready 表示已选择有效地址且 DTLS/SRTP 可用。收到 SDP answer、更新 ufrag、普通未提名检查都不能单独代表恢复完成。
- 同代次也可发生地址迁移，因此另有 `path_revision`；每次实际可用路径变化增加一次，包括同地址的新一轮 restart。
- Checking、Failed、Closed 不暴露旧地址。Ready 是传输状态，不证明已接收完整视频或恢复解码。
- 回调和 `NetworkPath()` 查询归属会话 I/O 线程。回调不得抛异常或同步停止、重协商当前会话；跨线程监控应在 owner 上复制快照再发布。
- 回调采用弱引用连接媒体适配器，避免延长房间/会话生命周期。

`WebRtcMediaTransport` 必须通过 `WebRtcSessionOptions::onNetworkPathChanged` 接收通知；`WebRtcService` 已完成此接线。自行构造适配器的调用方也需接线，不能依靠一次 Send 或收到一个 RTCP 包就自行恢复控制器可用状态。

## 当前已经接通的行为

1. 初始建立、restart 等待、路径可用、超时失败及关闭的生命周期通知。
2. restart/关闭期间禁用发送控制；Ready 时清除旧 PacketHistory、反馈适配状态、TWCC 新鲜度及 GCC 估计。
3. 保留发送 SSRC/时钟登记和单调递增的 TWCC 分配器，避免切网后重用序号匹配旧反馈；保留既有 RTP/DTLS/SRTP 会话身份。
4. 媒体适配器用路径 epoch 作废切网前已排队、尚未执行的异步发送，避免积压媒体被直接发到新路径。
5. 重复提名不重复重置估计；旧凭据和旧地址不能宣告新路径恢复。

异步 Send 的 Ok 仍表示入队成功，不保证最终写 socket。当前 sender 的发送历史记录时点与真实 socket 成功时点仍有差异，本轮没有重新设计异步发送完成回执；后续应在完成回执处结合路径代次记录历史与丢弃指标。RR 本身也不携带本地路径代次，新路径的早期 RR 可能覆盖了切网前的接收统计，不能声称所有旧测量已在协议层被精确隔离。

## 留给你继续写的核心任务

### 任务 1：恢复关键帧

入口已标在 `WebRtcMediaTransport::OnNetworkPathChanged`。保持该层只适配路径，不在这里遍历房间轨道：

- 在应用/endpoint 层订阅 Ready 事件，区分初始建立与恢复、发布与订阅。
- 对上行发布视频，在新路径可发 RTCP 后合并、限频发送关键帧请求。
- 对下行订阅视频，沿订阅关系向正确的上游发布者请求关键帧；不能把观众的 SSRC 当上游 SSRC。
- 使用现有 `KeyFrameRequestLimiter`，避免每个 STUN 包都发 PLI。

验收：一次恢复按视频轨道合并请求，音频不请求关键帧，同地址重复提名不再请求，移除轨道或关闭会话后任务安全退出。

### 任务 2：路径恢复统计

扩展纯状态或新增独立统计对象，在服务端单调时钟记录 restart 提交、新路径 Ready、新路径首个有效 RTP 的时间，输出次数、耗时和失败原因。不要记录 ICE 密码或恢复凭据。服务端不能仅凭收到 RTP 推断客户端已经恢复解码，客户端播放数据需要单独上报。

验收：超时与成功分别计数，重复 STUN 不重复记恢复，新一轮 restart 与同代次重绑定可区分。

### 任务 3：有界恢复策略与客户端协同

客户端主动检测持续 disconnected、串行重试 restart；服务端明确接受条件、失败类型、会话超时与保留期限。需要重新入会时返回可定位的错误，不通过延长 RTP 队列来掩盖断网。

当前默认成员保留窗口为 15 秒，从服务端检测到信令断开起算。旧 TCP 连接黑洞可能没有及时触发 close；信令心跳/在线成员接管需要单独设计，不能绕过原有成员绑定校验。

验收：信令与媒体同时断开时先 resume，再 restart；失败次数和整体期限有上限；过期回调不修改新会话。

### 任务 4：真实迁移数据

在 WebRTC session/transport/service 层扩展双 UDP 地址迁移试验，覆盖旧路径断流、新地址提名、同地址 restart、重复及错误提名、失败超时和信令同时断开。`test_sfu_endpoint.cpp` 可验证转发关系保留，ICE 事件应在 WebRTC 层验证。

下一步再用真实 UDP 和真实 DTLS/SRTP 对端测新路径首包及视频恢复时间，然后接手机切换移动网络。本轮测试使用真实 STUN 消息构造和认证处理，媒体加密边界使用测试替身，不是运营商网络或真实加密互通验收。

## 验证入口

```sh
cmake --build build/webrtc-integration \
  --target Packetia test_webrtc_session test_rtp_input test_weak_network_manual \
  test_webrtc_service test_webrtc_crypto test_datagram_transport -j4
ctest --test-dir build/webrtc-integration --output-on-failure \
  -R '^(test_webrtc_session|test_rtp_input|test_weak_network_manual|webrtc_service|test_webrtc_crypto|test_datagram_transport)$'
```

- `network/Test/test_webrtc_session.cpp`：restart 通知时机、旧凭据拒绝、重复提名、同代次重绑定、旧地址过滤、失败及关闭。
- `Rtsp/Test/test_webrtc_media_adapter.cpp`：真实 session 回调接到控制器，restart 期间清估计/禁反馈，旧异步发送作废，新路径恢复发送。
- `media/quality/Test/test_send_side_transport.cpp`：路径变化清旧历史、保留 TWCC 序号与 sender 登记、RR 回退及新 TWCC 生效。

2026-10-23 本机验证：主程序及上述测试目标构建通过，六个相关 CTest 入口通过。
`test_webrtc_crypto` 补充真实 DTLS/SRTP 回归，`test_datagram_transport` 覆盖传输与 UDP mux；本轮新增的地址迁移断言仍使用测试加密替身，没有把它描述成真实加密切网验收。

当前检出缺少 `log/Test` 和 `server/examples/WebRtcRoomDemo.cpp`，构建增加了存在性检查：缺少时跳过可选日志测试目录和独立 demo 目标，核心程序与本节回归目标仍可生成。没有新增日志测试或补造 demo 源码。构建仍有既有的重复库链接等警告。

这些是服务端框架的回归入口，不代表整条移动网络切换业务已经完成。
