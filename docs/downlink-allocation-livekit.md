# 对照 LiveKit 阅读下行带宽分配

本轮保留现有单层转发，将分配器组织成“准备、试分配、提交”三个阶段。
一次 `Allocate()` 处理一个接收连接上的订阅需求；链路预算来自发送侧控制器，
每条源流的码率需求来自 `SourceBitrateEstimator` 的 EWMA 结果或已有的码率提示。

## 先读 LiveKit 的分配入口

参考源码（2026-11-16 对照）：

- [StreamAllocator.allocateAllTracks](https://github.com/livekit/livekit/blob/master/pkg/sfu/streamallocator/streamallocator.go)
- [Forwarder.ProvisionalAllocatePrepare / ProvisionalAllocate / ProvisionalAllocateCommit](https://github.com/livekit/livekit/blob/master/pkg/sfu/forwarder.go)
- [TrackSorter.Less](https://github.com/livekit/livekit/blob/master/pkg/sfu/streamallocator/track.go)

先在 `allocateAllTracks()` 中找三个阶段，可以把核心循环简化为：

```text
对所有受控轨道：准备临时分配状态

从低空间层到高空间层：
    从低时间层到高时间层：
        按优先级遍历轨道：
            试分配这一层
            从剩余预算扣除本次新增的占用

对所有受控轨道：提交临时分配状态
```

它的目标是让更多轨道先以低层保持播放，再提升画质。
`Forwarder.ProvisionalAllocate()` 的返回值包含所需带宽的变化量：
假设临时状态已经分配 200 kbps，升级目标需要 500 kbps，本次新增占用为 300 kbps。
预算不足时，保留已成功试分配的低层，而不是先把它清掉。

## 再读本项目的三个函数

入口在 [DownlinkBandwidthAllocator.cpp](../media/quality/DownlinkBandwidthAllocator.cpp)，
数据结构在 [DownlinkBandwidthAllocator.h](../media/quality/DownlinkBandwidthAllocator.h)。

| 阶段 | 本项目函数 | 阅读时关注 |
| --- | --- | --- |
| 准备 | `Prepare()` | 保留 `Slot` 中的已提交状态，构建本轮独立的 `AllocationPlan` |
| 试分配 | `ProvisionalAllocate()` | 音频/豁免轨道先占用预算，视频按优先级试分配，结果只写入计划 |
| 提交 | `Commit()` | 应用暂停/恢复等待，复核当前占用，最后改变 `Slot::allowed` |

本轮每条视频只有一个候选档位，成本就是该流的 `required_bps`。
所以试分配阶段仍然是按优先级做整流准入；还没有 LiveKit 的跨轨道逐层提升效果。
三个阶段为后续增加真实可用层提供清晰的边界，当前调用方继续使用 `allowed` 执行转发控制。

`AllocationPlan::wanted` 是希望达到的目标，`Slot::allowed` 是已提交的带宽允许状态。
两者可能不同：需要暂停但尚未等满 300 ms 时，`wanted=false`、`allowed=true`。
尚未解除等待的轨道仍会占用发送预算。

## 用一次优先级切换理解提交

假设可用预算是 500 kbps，A、B 都各需要 500 kbps；当前只有 B 在转发。
暂停等待 300 ms，恢复等待设为 0，恢复余量设为 0，便于观察预算移交。

| 时刻 | 条件 | 临时计划 | 提交结果 |
| --- | --- | --- | --- |
| 301 ms | A 的优先级提高 | 希望 A 开、B 关 | B 尚在等待暂停，A 不能使用它占用的预算 |
| 601 ms | B 的暂停等待完成 | 仍希望 A 开、B 关 | 先暂停 B，复核释放的预算，再允许 A |

代码对应 `Commit()` 中的两轮处理：先处理暂停和计时，再统计仍然允许发送的需求，
然后按试分配时的优先级顺序恢复已满足等待条件的轨道。
恢复视频后，`DownlinkTrackManager` 仍然经过 `WaitingKeyFrame`，以关键帧恢复可解码画面。

## 读懂结果里的带宽变化

`DownlinkAllocation` 现在包含：

| 字段 | 含义 |
| --- | --- |
| `allowed` | 该轨道已提交的带宽允许状态；消费者是否静音、是否关闭仍由转发链路判断 |
| `bandwidth_requested_bps` | 当前符合转发条件且已允许的轨道需求；否则为 0 |
| `bandwidth_delta_bps` | 当前需求减去该轨道上一次提交的需求，单位 bps，使用有符号整数 |

示例：一条 500 kbps 视频从转发变为暂停，变化量是 `-500000`；恢复后为 `+500000`；
需求和状态都不变时为 `0`。源流的测量码率变化或可转发条件变化，也会反映在变化量里。
轨道移除或 `Clear()` 后，再加入的同名轨道从零开始记账。

恢复需要的 20% 余量只用于准入检查：500 kbps 视频恢复时可能需要预留 600 kbps，
提交后的 `bandwidth_requested_bps` 仍是 500000。它是源流需求估计，不是网卡实测发送速率。
这些结果按轨道输出；删除轨道不会额外输出一条释放记录，总预算在每轮重新计算。

## 本轮保留的策略与下一步

默认仍保留 85% 可用预算、300 ms 暂停等待、1000 ms 恢复等待、20% 恢复余量。
新轨道初始允许转发；无效或过期预算保留既有允许状态并重置切换计时。
这些是本项目当前策略，并非 LiveKit 的参数约定，也不构成始终不超预算的硬保证。

下一轮若增加分层，需要先提供真实的可用层、各层总码率、订阅者期望层，以及层切换时的
RTP/关键帧处理。届时可以扩展试分配阶段，让各轨道先尝试最低层，再按新增带宽逐层升级。
当前没有宣称支持 Simulcast/SVC 的媒体协商或分层转发。

## 验证

```sh
cmake --build build/webrtc-integration --target test_weak_network_manual PacketiaSfu -j 6
PACKETIA_LOG_MODE=console build/webrtc-integration/media/quality/tests/test_weak_network_manual \
  --gtest_filter='DownlinkAllocator.*:MetricSampling.*'
```

测试覆盖优先级、音频预留、暂停与恢复等待、试分配后实际预算复核、带宽变化量、
恢复余量与实际需求区分、输入顺序、未知码率、轨道移除和大整数边界。
