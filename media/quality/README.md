# 发送侧 GCC 接入

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
RR 仅接受已注册发送 SSRC 的报告，jitter 按该轨时钟换算；收到 TWCC 后，RR 只补充有效 RTT，
避免单轨 RR 覆盖整条 Transport 的包级带宽和丢包估计。

发送历史最多保留最近 2048 次记录，序号匹配采用最近的 16 位回绕周期，反馈窗口应小于 32768 个序号。
每次发送的首次反馈会消费历史，重复和重叠反馈不再推进 GCC；已报告丢失的包随后到达时，
本阶段不再回补该次发送的估计。NACK 重传拥有独立的 TWCC 序号和历史记录。

当前闭环到达**控制输出接口**。Pacer 定时发包、编码器调码率、FEC 执行、探测和反馈超时回退尚未接入；
`SendResult::Ok` 的时间沿用 Transport 的发送接受语义，并非底层 socket 完成发送的时间戳。
房间订阅业务和 RTSP PLAY 也仍需继续实现。

`Test/test_send_side_transport.cpp` 使用真实 RTP/TWCC/RR 字节及内存 Transport 验证发送与反馈链路，
加入现有 `test_weak_network_manual` 测试目标。
