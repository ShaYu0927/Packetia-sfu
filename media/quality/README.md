# 弱网第一版与发送侧 GCC 接入

当前已有丢包恢复和网络反馈链路，拥塞控制计算能输出目标码率，但业务尚未根据这个结果执行平滑发送或媒体降码率。
这里的 GCC 是 Packetia 自研的时延趋势、接收速率、丢包与码率策略组合，不是完整移植的 libwebrtc GoogCC。

## 当前能力

| 能力 | 实现位置 | 当前状态 |
| --- | --- | --- |
| 上行丢包检测、NACK 重试 | `Rtsp/Rtp/NackRequester`、`RtpVideoTracker` | 已接入视频收包；重排等待、RTT 重试间隔、次数/年龄上限及失败回调 |
| 下行缓存与 NACK 重传 | `Rtsp/RtpSenderTrack` | 缓存原 RTP，保留 RTP 序号、重新分配 TWCC 序号；默认缓存 512 包、每包最多重传 3 次、最小间隔 30 ms |
| SRTP 重传 | `protocol/WebRtc/SrtpTransport` | 允许发送端重新保护缓存包；接收端继续认证和防重放；本次修复前同序号重传会被发送端拒绝 |
| 关键帧恢复 | `RtpSenderTrack`、`SfuEndpoint` | 下行 PLI/FIR 回到发布者；上行 NACK 恢复耗尽后请求 PLI；发送轨 PLI 默认间隔 1 秒，跨订阅者统一合并仍待补 |
| 接收质量、RR | `SfuEndpoint::EvaluateReceiveQuality` | 统计丢包/抖动/RTT；生成 RR、质量等级和策略；由上游 SR 回调触发 |
| 下行 TWCC 反馈处理 | `SendSideController` | 匹配发送历史、更新带宽和丢包估计，音视频共用 Transport 状态 |
| OU 时延检测与码率控制 | `bwe/ou_trend_detect`、`remote_bitrate_controller` | 已适配参考 BWE 算法；结合时延趋势、接收速率、RTT 和丢包输出目标码率 |
| TWCC 过期后 RR 回退 | `SendSideController` | 已实现：最近有效 TWCC 未满 1000 ms 时，RR 仅补充 RTT；过期后新的 RR 可驱动回退策略 |
| 发送执行 | `NetworkControlUpdate` 输出接口 | 业务未注册消费回调；没有实际 Pacer、探测、逐订阅码率/分层执行和 FEC |

## 两个方向的业务链路

上行是发布者到 SFU：视频收包发现序号缺口，经过重排等待后向发布者发送 NACK；重传包进入解包/组帧，恢复耗尽后请求关键帧。
目前 NACK 的推进依赖收到 RTP 时的即时处理及 SR 回调中的 `TickNack`，还没有独立固定周期时钟；RTP 和 SR 都停顿时不能及时推进重试。

下行是 SFU 到观众：每个订阅发送轨缓存自己改写后的 RTP。观众 NACK 到达后，SFU 使用下行序号查找本地缓存并重传；观众 PLI/FIR 则通过订阅关系转回原发布者。
这不是简单地把观众的下行 NACK 序号转交给发布者，上下游 RTP 序号可能不同。

## 发送侧控制入口

`MediaTransportBase` 为每条下行 Transport 按需创建一个 `SendSideController`。
`RtpSenderTrack` 构造时自动注册发送 SSRC，并使用该控制器的 TWCC 序号分配器。
不同 Transport 各自维护历史和估计，共用一个 Transport 的音视频与重传共享这些状态。

```text
RtpSenderTrack::InputRtpPacket / OnRtcpNack
  → 写入下游协商的 TWCC extension
  → SendRtp 返回 Ok
  → SendSideController::OnPacketSent → PacketHistory

MediaTransportBase::PublishPacket(RTCP)
  → SendSideController::OnRtcpPacket
  → TWCC 解析 → 16 位序号展开 → 匹配并消费发送历史
  → GoogCcNetworkController → NetworkControlUpdate
  → SetUpdateCallback 注册的消费者
  → 原有 PacketSink / Endpoint 继续接收 RTCP
```

创建发送轨时，仍需显式传入**下游协商**的 `transport_cc_extension_id`（当前支持 1～14）。
默认值 0 禁用 TWCC，不消耗 transport sequence，也不记录 TWCC 历史；RR 仍可驱动回退估计。
不要把上游 SDP 的 extension ID 直接用作下游协商结果。

通过 `transport->GetSendSideController()` 获取控制器，再调用 `SetUpdateCallback` 接收目标码率、
Pacer 配置等输出。自定义 `IMediaTransport` 可以返回空指针，保留原来的发送行为。
状态查询使用 `GetNetworkState()`，不会推进算法。回调在内部锁释放后执行，可查询状态；
它在触发事件的线程同步执行，消费者应快速处理，并自行串行化跨线程的应用动作。
回调不应抛出异常，也不应强引用所属 Transport。

网络离开 Connected 状态时，清空历史及估计，并通过空 `NetworkControlUpdate` 通知消费者清除旧状态。
恢复后等待新反馈；序号继续递增，避免旧反馈立即匹配到新包。
RR 仅接受已注册发送 SSRC 的报告，jitter 按该轨时钟换算；TWCC 仍有效时，RR 只补充有效 RTT，
避免单轨 RR 覆盖整条 Transport 的包级带宽和丢包估计。TWCC 超过 1000 ms 后，收到新的 RR 才推进回退，
这不等于完全没有反馈时会自动降码率；后者仍需周期超时处理。

发送历史最多保留最近 2048 次记录，序号匹配采用最近的 16 位回绕周期，反馈窗口应小于 32768 个序号。
每次发送的首次反馈会消费历史，重复和重叠反馈不再推进 GCC；已报告丢失的包随后到达时，
本阶段不再回补该次发送的估计。NACK 重传拥有独立的 TWCC 序号和历史记录。

当前闭环到达**控制输出接口**。Pacer 定时发包、媒体降码率、FEC 执行、探测和完全无反馈时的周期超时处理尚未接入；
`SendResult::Ok` 的时间沿用 Transport 的发送接受语义，并非底层 socket 完成发送的时间戳。
WebRTC 房间订阅业务已接通，RTSP PLAY 尚未实现。

SFU 当前转发单个编码，并不拥有浏览器的编码器；一个观众网络变差，不能简单让所有观众共享的发布流按该观众带宽降码率。
后续必须明确媒体策略：发布者侧码率控制、可选的多编码/分层切换，或视频暂停等。Pacer 只平滑发送，无法持续容纳高于可用带宽的输入。

上行 `TransportFeedbackGenerator` 有独立实现和字节测试，但尚未在发布者接收路径按协商扩展 ID 记录到达时间、周期发送 TWCC。
`PacketRecoveryController`、`VideoRecoveryController` 目前仍是空壳文件；已运行的恢复逻辑位于 NackRequester、发送轨和 SfuEndpoint，不能按文件名判断接入完成。

## 参考 BWE 算法的集成

参考代码中的 `OUTrendDetect + RemoteRateControl` 已适配到现有 `TransportBweController`。
保留 Packetia 的 `TransportFeedback`、`BitrateConstraints` 和 `NetworkControlUpdate` 接口，
不依赖参考代码的 `engine_configurations`、旧 WebRtc 类型和 trace 系统，也不重复运行原来的 `LossBasedController`。
这是对其核心思路的适配；旧代码的音视频独立参数、TMMBN 初始化和报告系统没有整体移植。

```text
观众 RTCP TWCC → 发送历史匹配 → TransportBweController
  ├─ 按发送顺序：OUTrendDetect 检测到达间隔与发送间隔的差值
  ├─ 按到达顺序：IncomingBitrateEstimator 计算接收速率
  └─ RemoteBitrateController：时延状态 + 接收速率 + 丢包 + RTT
       → NetworkController → 目标码率 / Pacer 建议 / 网络质量
观众 RTCP RR → RTT 补充（TWCC 有效时）或原有 RR 回退策略
```

- **启动与约束**：最小、初始和最大码率统一使用 `BitrateConstraints`，要求 `0 < min <= start <= max`。
  正常启动先输出配置的初始码率；接收速率有效后观察约 2 秒再初始化估计，明确检测到排队拥塞时可提前初始化。
  观察期结束不会撤销正在进行的丢包/排队降码率。
- **时延检测**：累积 `Δ到达时间 − Δ发送时间`，按经过的时间衰减，扣除近期抖动与包大小变化的发送耗时。
  正常时允许升码率，轻微排队时保持，持续排队超过阈值后降码率。近期抖动最大值使用有界单调队列维护，
  避免逐包扫描历史。检测支持 32 位发送毫秒时钟回绕，并跳过倒序样本。
- **码率控制**：通过 Increase / Hold / Decrease 状态和容量区域控制变化速度。
  升码率按实际经过的时间和 RTT 推进，高 RTT 放慢增长；重复或倒退的反馈时间不会继续推进。
  丢包升高时先降码率，再等待约 1.5～2.5 秒观察变化；观察期间暂停增长。
  新的排队拥塞会取消旧的丢包恢复基准，避免恢复到已经不适用的码率。
- **指标含义**：接收速率采用最近 500 ms 的包字节统计，样本跨度不足 100 ms 时为 0。
  `recv_bitrate_bps` 是实测接收速率，`target_bitrate_bps` 是发送建议，两者分别输出。
  `bandwidth_bps == 0` 表示估计尚未初始化；此时目标码率仍有值。初始化后的输出也不等于已经测得链路最大容量，
  当前还没有主动探测。
- **重置与边界**：路由变化事件和网络断开会重置时延、接收速率、容量、丢包基线、RTT 和启动状态。
  码率始终限制在配置范围内；低码率减法、100% 丢包及 Pacer 建议计算避免下溢、除零和溢出。

码率单位为 bit/s，时延/RTT 为 ms，丢包率为 0～1；控制推进使用本地单调时钟。
TWCC 的接收时间来自观众的时钟，检测器只比较两端的时间差，不把两端绝对时间相减作为单程时延。
RR 回退仍使用原来的质量分级策略，这次时间/RTT 驱动的码率算法用于 TWCC 路径。

## 后续完成顺序

1. 为 NACK 重试和反馈生成接入有明确线程归属的周期时钟；覆盖媒体停顿、RTCP 丢失、关闭注销和 ICE restart。
2. 补齐发布者到 SFU 的 Transport 级 TWCC 生成与发送，音视频共用到达记录及反馈序号空间，使用本次协商的 extension ID。
3. 实现每条下行 Transport 的有界 Pacer，消费控制输出；定义音频/视频/重传调度、排队时间、满队列与关闭行为。
4. 接通适合 SFU 的媒体码率/分层策略，并验证一个弱网订阅者不影响其他正常订阅者。
5. 在独立网络环境注入丢包、抖动、限速和短时断网，观察实际 NACK/PLI/TWCC、队列和解码恢复。FEC、探测、RTX 作为后续扩展。

## 验证

`Test/test_send_side_transport.cpp` 使用真实 RTP/TWCC/RR 字节及内存 Transport 验证发送与反馈链路，
加入现有 `test_weak_network_manual` 测试目标。
`Test/test_bwe_rate_control.cpp` 覆盖启动约束、不同反馈频率、RTT、丢包观察与排队冲突、
孤立抖动、时钟回绕、路由/断网重置及数值边界；另用真实 TWCC 字节验证发送历史匹配后的降码率和恢复。

```sh
cmake --build build/webrtc-integration \
  --target Packetia test_weak_network_manual test_rtp_input test_webrtc_session test_webrtc_crypto --parallel 4
ctest --test-dir build/webrtc-integration --output-on-failure \
  -R '^(test_weak_network_manual|test_webrtc_session|test_webrtc_crypto)$'
build/webrtc-integration/Rtsp/Test/test_rtp_input \
  --gtest_filter='RtcpChainTest.*:SfuConference.SubscriberOwnsSenderAndFeedbackReturnsToPublisher:SfuConference.RebuildsDownstreamMidAndTwccInsteadOfCopyingUpstreamIds'
```

真实 SRTP 回归模拟原包丢失、同 RTP 序号而新 TWCC 序号的重传到达、原包迟到和重复重传，确认可恢复丢失包且不放松接收防重放。
2026-10-12 验证结果：33 项弱网测试、27 项 RTCP/SFU 回归、WebRTC 会话与 DTLS/SRTP 测试通过，主程序编译通过。
单元/组件测试通过不代表已经完成真实浏览器弱网质量验收；下一步按上述顺序补齐执行链路，
再用独立网络环境验证限速、抖动、丢包和短时断网后的队列及解码恢复。
