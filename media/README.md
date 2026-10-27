# 媒体模块边界

## 数据与依赖

| 层 | 文件或接口 | 职责 |
| --- | --- | --- |
| 公共类型 | `types/MediaTypes.h`、`CodecNames.h` | 媒体类型、编码身份、方向与编码名称解析 |
| 业务轨道 | `track/TrackMetadata.h`、`MediaTrack.h`、`MediaTrackFwd.h` | 业务 ID、来源、静音、视频层等元数据及值快照 |
| 通用轨道 | `track/MediaStreamTrack.h` | core 使用的编码、音频采样率与声道描述及校验 |
| 流管理 | `core/MediaStream`、`MediaStreamRegistry` | 流注册、状态与轨道到 Endpoint 的绑定 |
| RTP 值类型 | `rtp/RtpStreamContext.h` | 每个媒体段中的候选编码，包含 PT、RTP 时钟和 FMTP |
| RTP 选择 | `rtp/RtpTrackBinding.h/.cpp` | 选择当前链路支持的候选项，生成通用轨道描述 |
| SDP 适配 | `rtp/SdpTrackBinding.h/.cpp` | SDP 到 RTP 候选快照和头扩展参数的转换 |
| 接收器兼容适配 | `Rtsp/RtpTrackAdapter.h/.cpp` | 将选中的 RTP 参数转换为旧接收器的 `::TrackInfo` |
| 统计输入 | `stats/IMediaStatsSink.h` | 生产者依赖的统计注册、注销和事件接口 |
| 状态事件 | `core/MediaEvent.h`、`EventBus.h` | 状态事件描述和同步订阅分发，不承载 RTP 收发器或统计实现 |

公共类型、业务元数据、通用轨道、RTP 值类型与选择逻辑都不包含 SDP、RTSP 或网络头文件。
`MediaFrameTypes.h` 只补充帧专用类型，基础枚举来自 `MediaTypes.h`。
`CodecNames.h` 统一处理 AAC/MPEG4-GENERIC、H265/HEVC 及大小写别名；识别格式不等于接收链路支持。
当前接收选择策略仍为视频 H264，音频 Opus、PCMU、PCMA、AAC。

`TrackMetadata` 替代原业务 `media::TrackInfo`，避免与旧接收器的全局 `::TrackInfo` 混淆。
业务轨道使用字符串 ID，core 流内轨道使用数值 ID，两者尚未统一映射。
`MediaTrack` 读取元数据时加锁并返回自有值；调用方声明句柄时只需 `MediaTrackFwd.h`。

## 协商参数的唯一来源

RTSP 的 `ApplySdp` 一次解析完整 SDP，生成只读 `RtpStreamContext`。
每个媒体段保存候选列表，选中项只保存列表索引；不再另存一张可独立修改的 `TrackInfo` 参数表。
音视频可以使用相同 PT，查找必须带轨道索引，不建立跨轨道的 PT 总表。

```text
RTSP SDP
    → RtpStreamContextBuilder：一次生成候选快照
    → SelectRtpPayload：选择候选索引
        ├─ DescribeMediaTrack → MediaStream 的通用轨道描述
        └─ ToLegacyRtpTrackInfo → 旧 RTP 接收链路

WebRTC 单个 SDP 媒体段
    → BuildRtpTrackInfo 兼容入口
    → 相同的解析、选择与接收器适配
```

`clock_rate` 是 RTP 时间戳时钟；`MediaStreamTrack::sample_rate` 是音频采样率，视频为 0。
当前支持的音频接入沿用 SDP 的采样率约定，没有在本轮新增音频配置解析或 SBR 适配。
通用描述是协商结果的投影，不保存 PT、头扩展或原始 SDP FMTP。

RTSP control 查找、TCP channel 占用和连接发布生命周期仍由 `RtspMediaSession` 管理。
替换时先构建并验证所有候选、选择和通用描述，再统一提交；失败保留原描述和绑定。
同连接再次发布时复用只读协商快照及选中索引，使用新的 core 流 ID。

## 构建与执行边界

- `packetia_media_types`：公共头文件。
- `packetia_media_core`：流管理，只依赖公共类型和线程库。
- `packetia_media_rtp`：纯参数选择和描述转换，只依赖公共类型。
- `packetia_sdp_media_adapter`：SDP 解析适配，依赖现有 SDP 库与日志。
- `packetia_media_stats`：独立编译统计实现和指标计算，不依赖接收器或房间。

RTSP 适配对象仍在所属连接 EventLoop 串行调用；Endpoint 媒体处理仍走现有 MediaWorker。
参数快照本身不调度任务、不写网络、不持有房间。帧继续通过 `EncodedFrameRouter` 分发。
`EventBus::Publish` 在发布者线程同步调用订阅者，不会自动切换线程。

统计的 RTP/RTCP/帧事件处理目前仍是空实现，尚未接入媒体链路；本轮仅完成依赖隔离、
公共类型统一和独立编译。
WebRTC 房间发布订阅关系仍由房间层管理。本轮未新增 RTMP、SRT 或跨协议输出。

## 房间轨道事件

每个 `Room` 拥有一个 `EventBus`，通过 `Events()` 订阅。
`TrackPublicationEvent.h` 定义 `TrackPublishedEvent`、`TrackUnpublishedEvent`。
成功发布、显式取消发布、发布者离房和房间关闭都会在状态提交及对应清理后产生事件。
重复操作或被拒绝的发布不会产生成功事件。发布事件表示轨道已登记，不保证已经收到媒体。

事件快照包含房间 ID、发布者 ID、轨道元信息、发布实例 `publication_id` 和变更 `revision`。
两种编号只在当前房间实例内有效；同 ID 重新发布会获得不同发布实例编号。
它们是变更事实，不是操作命令。路由清理继续由 Room/Endpoint 的直接调用保证先后顺序。

Bus 在发布线程同步分发，回调在房间锁外执行；异步处理必须复制快照，并切换到目标执行器。
RAII 订阅句柄可晚于 Bus 销毁；退订会使分发快照中的槽失效，
但已经通过 active 检查的回调仍可能执行，退订不等待它们结束。
订阅回调捕获业务对象时使用弱引用。回调异常被隔离，`DispatchResult.failed` 返回失败数量；
房间发布事件时会记录失败日志。Bus 本身没有重放和自动重试能力。

`RoomSignalHandler` 订阅发布和移除事件，回调只更新版本槽；owner 的 Tick 合并变化，
经现有有界响应通道发送 `tracks_changed` 最新列表。通知积压使用通道已有的字节数和消息数限制。
恢复连接重新推送当前列表，离房和停服解除订阅。详见 `server/signaling/README.md`。

当前未接入通用 `MediaStream`/RTSP 流状态事件、静音媒体控制、动态权限、订阅意图重试，
也未增加轨道级 Open/Closing/Closed 生命周期。这些不能从发布/移除通知推断为已经实现。
