# 会议合成框架

当前只提供任务框架，**还不能生成多人合成视频**。没有实现解码、时间对齐、画面布局、混音或编码。服务已接入服务器的统一启停管理，默认关闭。

一个 `ConferenceMixer` 表示一个会议的合成任务。它订阅现有的编码帧路由，把指定轨道的帧放入有界队列，在独立线程调用 `IMixBackend`，再把后端产生的编码帧发布到另一条路由。

```text
参会人编码帧 → 输入 EncodedFrameRouter
                       ↓ 按 endpoint_id + track_id 筛选
                ConferenceMixer 队列
                       ↓ 独立线程
                  IMixBackend（待实现）
                       ↓ H264 视频 / AAC 音频
               输出 EncodedFrameRouter
                       ↓ 后续接入
                RecordingService → MP4
```

## 文件职责

| 文件 | 职责 |
| --- | --- |
| `MixTypes.h` | 输入名单、会议和输出标识、队列配置、状态与统计 |
| `ConferenceMixer.h/.cpp` | 订阅、筛选、排队、工作线程、启停与错误处理 |
| `ConferenceMixService.h/.cpp` | 全局开关、会议任务所有权、关闭时停止全部任务、重新开启 |
| `IMixBackend.h` | 后续编解码、同步、拼画面、混音的实现接口 |
| `Test/` | 使用模拟后端验证框架，不依赖 FFmpeg 或 GTest |

## 接入约定

- 业务层显式填写 `MixConfig.inputs`，每项包含参会人标识、端点 ID 和轨道 ID。通过服务器的共享配置启动任务时，还必须提供发布源的 `session_id/stream_id`，用于逐流许可检查。同一人的音视频分别配置；不同端点可以使用相同轨道 ID。不能直接把输入帧的 `session_id` 当作会议 ID。
- 一个任务的名单固定。后续再讨论成员进出、布局切换、静音等业务；当前调整配置需要创建新任务。
- 输入、输出必须是不同的路由对象。输出端点使用独立的非零 ID，不能与任何输入端点相同。框架将输出的 `session_id` 设为 `room_id`，`stream_id` 设为 `output_stream_id`。
- 用 `shared_ptr` 持有任务，控制层保留所有权直到 `Stop()` 完成。`Start()`、`Stop()` 和最终销毁由控制线程执行，不能在后端或输出订阅回调中调用，否则可能造成线程等待自身。
- 后续创建 `RecordingService` 时传入输出路由。先初始化并启动录制，再启动合成；关闭时先停止合成，等待尾帧输出，再停止录制。
- `ConferenceMixService` 实现 `IService`，由服务器实际使用的 `ServerLauncher` 管理。全局开关与单个会议任务分开：打开服务只允许创建任务，不会自动开始合成。

## 全局开关与会议任务

启动时可设置 `PACKETIA_CONFERENCE_MIX=1`，或在 `ServerConfig` 中设置 `conference_mix_enabled = true`。
运行中的控制线程调用 `app.SetServiceEnabled(service::ServiceType::ConferenceMix, false)`，会停止所有会议任务、退订输入、排空队列并等待工作线程退出；返回成功后才更新开关状态。传入 `true` 可以重新开启服务。

服务重新开启后，通过 `app.StartConference(config, std::move(backend))` 创建新任务；`app.StopConference(room_id)` 只结束指定会议，不关闭整个服务。每次开始都传入新的后端实例；缺少后端时创建任务失败。旧任务不会自动恢复。

输入还受[流级配置](../../config/README.md)约束。当前不支持后端动态移除输入，因此运行时禁用某路输入会停止包含它的会议任务；其他会议继续运行。重新允许该流后，需要重新创建任务。

`app.MixedFrameRouter()` 暴露合成输出路由，后续由应用连接录制订阅者；当前服务器的录制开关控制已有的原始流录制，尚未自动保存合成输出。各会议的输出端点 ID 由应用统一分配，避免冲突。

环境变量只在进程启动时读取。运行时开关目前是 C++ 接口，尚未提供 HTTP/WebSocket 管理命令或配置文件自动重载。详见[服务配置说明](../README.md)。

以下展示配置方式；省略的后端和业务生命周期需要后续实现：

```cpp
service::mix::MixConfig config;
config.room_id = "meeting-001";
config.output_stream_id = "conference-mix";
config.output_endpoint_id = 10001; // 由应用分配，避免与其他端点冲突。
config.inputs = {
    {"zhangsan", 101, 0, "publish-zhangsan", "camera"}, // 使用实际发布源标识。
    {"lisi",     102, 0, "publish-lisi", "camera"},
};
// app 已启动，并启用了合成服务和这些输入流的合成许可。
// backend 必须是后续实现的 IMixBackend；未传入时创建任务失败。
if (!app.StartConference(config, std::move(backend))) {
    // 向控制层报告启动失败。
}
// 会议结束时调用 app.StopConference(config.room_id)。
```

## 后端契约

`Start → InputFrame / Tick → Stop` 全部在同一个工作线程串行执行。`Tick` 即使没有输入也会触发，参数是任务启动后的单调微秒数；它不替代后端需要实现的媒体时间轴和音视频同步。

输出回调只能在 `InputFrame`、`Tick` 或 `Stop` 内同步调用，不能在 `Start` 中输出，也不能从后端自建线程调用。回调返回是否至少有一个订阅者接受了帧，不代表录制已经落盘。`Stop` 必须不抛异常，在正常停止时排空编码器，并释放输出回调；初始化失败后也会调用它来清理部分资源。

后端负责输出完整的 H264/AAC 帧，提供稳定且互不冲突的音视频轨道 ID、正确时间戳与时间基，以及录制需要的元数据。H264 需要符合现有录制器要求的 Annex B 数据、SPS/PPS、关键帧标记和宽高；AAC 需要 AudioSpecificConfig、采样率、声道数等。先遵守现有录制链路不使用 B 帧的约束。框架仅做基础帧检查，不保证码流可解码。

## 状态和边界

- 状态主路径为 `Created → Starting → Running → Stopping → Stopped`，异常进入 `Failed`，原因由 `LastError()` 返回。任务不可重启。
- `SubmitFrame` 只筛选和入队，不执行编解码或等待队列腾出空间。正常停止拒绝新帧、处理已接收帧、调用后端收尾，再等待线程退出。
- 队列同时限制帧数和字节数，不包含当前正在处理的一帧。当前超限会使任务失败并清空待处理帧，避免静默丢弃压缩帧破坏解码参考关系。后续可设计丢帧、关键帧请求和恢复策略。
- 后端或输出订阅者异常使任务失败；失败后的尾帧不会继续发布。`Stop()` 返回后不会再有本任务输出。后端调用必须能及时返回，否则停止仍会等待它。
- `Stats().published` 是发布次数，不是成功写入 MP4 的帧数。

## 验证框架

已加入主工程的 `BUILD_TESTING`。也可以单独构建，不需要 FFmpeg/GTest：

```bash
cmake -S service/ConferenceMixService/Test -B build-mix-tests
cmake --build build-mix-tests
ctest --test-dir build-mix-tests --output-on-failure
```

测试涵盖轨道筛选、独立线程、空闲定时调用、输出标识、停止排空与收尾、缺少后端、异常隔离，以及帧数/字节数超限。模拟后端仅用于测试路由和生命周期，不是合成实现。
