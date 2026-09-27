# 各路独立 fMP4 切片

每个 `(session_id, stream_id)` 独立录制并连续生成可单独播放的 fMP4 文件。`session_id` 可以由业务层映射为会议 ID；`stream_id` 必须在这个会话内唯一，例如 `zhangsan-camera`、`lisi-camera`、`lisi-screen`。同一录制最多包含一条 H264 视频和一条 AAC 音频，不支持 B 帧。音视频可来自不同 endpoint，但必须由上游明确关联到同一发布流。

## 业务模型

例如会议 A 中张三发布摄像头，李四发布摄像头和屏幕共享，将形成三组独立片段：

```text
meeting-A
├─ zhangsan-camera → 片段 1、片段 2、……
├─ lisi-camera     → 片段 1、片段 2、……
└─ lisi-screen     → 片段 1、片段 2、……
```

三路可以有不同的开始时间和切点。停止张三的流不会停止李四的流。会议归属通过索引保存，磁盘文件名不承担业务关联。

| 标识 | 含义 |
| --- | --- |
| `session_id` | 上游提供的业务会话，可映射为会议或通话 |
| `stream_id` | 会话内唯一的发布流；一个参与者可以有多路 |
| `recording_id` | 一路流的一次录制实例；停止后重启会变化，不是整个会议的任务 ID |
| `segment_id` | 一份片段的唯一标识 |
| `sequence` | 当前逐流录制实例内的片段序号，从 1 开始 |

当前提供逐流控制与按会话查询，不维护房间成员名单。需要录制全部新发布流时可设置 `stream_defaults.recording=true`；房间级选择、批量启停及参与者名称映射由上层业务接入。

## 代码结构

```text
EncodedFrameRouter
  → RecordingService       接收策略、逐流控制、索引初始化与查询
  → RecordingDispatcher    有界队列、同流固定分片线程
  → RecordingSession       逐流实例生命周期
  → RecordingInstance      录制器适配
  → Mp4Recorder            轨道发现、时间对齐、排序、切点与片段状态
      ├─ Mp4Writer → LocalFileIO    FFmpeg 封装与文件写入
      └─ RecordingCatalog          SQLite 索引事务
```

核心入口是 [RecordingService.h](RecordingService.h)，切片逻辑在 [Mp4Recorder.cpp](Mp4Recorder.cpp)，查询数据结构在 [RecordingRecords.h](RecordingRecords.h)，数据库操作在 [RecordingCatalog.cpp](RecordingCatalog.cpp)。`ServerApp` 对上层提供控制与查询方法。索引写入在录制线程同步完成，成功后才发送完成事件，外部事件接收器无需承担索引入库职责。

## 配置与控制

```json
{
  "services": {"recording": true},
  "stream_defaults": {"recording": false},
  "recording": {
    "directory": "recordings",
    "segment_ms": 60000,
    "discovery_ms": 1000,
    "reorder_ms": 100,
    "idle_timeout_ms": 3000
  }
}
```

通过 `PACKETIA_CONFIG` 加载 JSON，或在创建 `ServerApp` 前设置对应字段。`segment_ms=0` 关闭自动切段；目标片长依据媒体时间，视频在达到目标后的可独立解码关键帧处切段，因此实际时长可能更长。`reorder_ms` 是跨轨道排序等待窗口（0～5000 毫秒），超过窗口才到达且落后于已写媒体的包会被丢弃并计入统计。排序缓存与轨道发现缓存共用 `max_pending_bytes` 上限。

| `recording` 配置项 | 默认值 | 作用 |
| --- | --- | --- |
| `directory` | `recordings` | 媒体文件根目录 |
| `index_path` | 空，自动使用 `.index/recordings.sqlite` | SQLite 路径；自动路径相对媒体根目录 |
| `segment_ms` | `60000` | 目标片长，单位毫秒 |
| `discovery_ms` | `1000` | 初始轨道发现窗口 |
| `reorder_ms` | `100` | 跨轨道排序等待窗口 |
| `idle_timeout_ms` | `3000` | 断流多久后收尾当前实例 |
| `max_streams` | `16` | 并行逐流实例上限 |
| `worker_count` | `2` | 录制工作线程数 |
| `max_pending_bytes` | `67108864`（64 MiB） | 全部实例共享的发现与排序缓存预算 |

其他队列保护参数见 [RecordingOptions.h](RecordingOptions.h)。时间与资源参数在服务初始化时使用；运行中的逐流启停通过下面的控制接口完成。

```cpp
config::StreamKey zhangsan{"meeting-A", "zhangsan-camera"};
config::StreamKey lisi{"meeting-A", "lisi-camera"};

// app 已成功 Start；从控制线程调用并检查 bool 结果。
bool started = app.StartRecording(zhangsan);
bool other_started = app.StartRecording(lisi);
bool stopped = app.StopRecording(zhangsan);
bool restarted = app.StartRecording(zhangsan);
```

开始接口允许后续媒体进入；文件需等待有效轨道参数和关键帧。重复开始不创建额外实例。停止会关闭该流的新帧接收、排空已接收数据和排序缓存、收尾最后一段，并同步提交该段索引；其他流继续录制。停止后再次开始使用新的逐流 `recording_id`，不会续写原文件。接口只更新录制开关，保留 AI 和会议合成配置。

不要从媒体、录制事件或工作线程回调中调用这些同步控制接口。开始时全局录制必须开启。`StopRecording` 的返回值描述本次收尾；之前已经终止的失败实例通过失败事件、索引和 `Stats()` 查询。

## 文件与索引

默认结构：

```text
recordings/
  .index/recordings.sqlite
  <run>-<generation>-1.mp4
  <run>-<generation>-2.mp4
  .<run>-<generation>-3.mp4   # 正在写入的临时文件
```

文件名仅使用内部生成的 ID，不拼接会议名、参与者名或其他用户输入。写入前建立 `Writing` 索引；成功关闭并重命名后记录大小、轨道、媒体时间范围和 `Completed` 状态。失败记录为 `Failed`，默认播放查询不会返回它。临时文件保留供诊断，不作为成功录像发布。索引写入失败会使当前录制失败，不会发送成功完成通知。

SQLite 默认位于 `<directory>/.index/recordings.sqlite`，可用 `recording.index_path` 指定独立位置。录像放到 NAS 挂载目录时，应将 SQLite 留在本地磁盘；索引保存相对于录像根目录的路径。数据库使用 WAL 和同步事务，服务可停止后查询，也可重新初始化或直接打开 `RecordingCatalog` 查询历史记录。一次只由一个录制服务拥有该录像目录和索引。

每段记录包括：逐流录制 ID、片段 ID、会话 ID、流 ID、序号、媒体起止时间、UTC 起止时间、相对文件路径、大小、轨道（endpoint/track ID、编码、分辨率或采样率/声道）、状态和错误信息。

## 查询

```cpp
auto streams = app.ListRecordedStreams("meeting-A");

service::SegmentQuery query;
query.session_id = "meeting-A";
query.stream_id = "zhangsan-camera";
query.limit = 100;                  // 1～1000，offset 用于分页
// query.recording_id = ...;        // 限定某次开始到停止的逐流实例
// query.from_ms = ...;             // UTC 毫秒，查询与 [from_ms, to_ms) 相交的片段
// query.to_ms = ...;
auto segments = app.QueryRecordingSegments(query);
```

查询只访问数据库，不扫描 MP4 目录；默认只返回 `Completed`，按时间、录制 ID、序号排序。管理界面可设 `completed_only=false` 查看写入中和失败的片段。查询参数无效、索引未初始化或数据库读取失败会抛出异常，不应作为“没有录像”处理。`RecordingCatalog` 也可单独打开数据库用于离线查询。

`ListRecordedStreams` 返回已经产生片段索引的逐流录制实例。相同 `stream_id` 停止后再开始，列表可以出现多个不同的 `recording_id`；开启接收但尚未生成片段的流不在此列表中。`relative_path` 相对于 `recording.directory`，不是可直接对外访问的下载 URL。

`SegmentStarted`、`SegmentCompleted`、`SegmentFailed` 事件新增可选 `segment` 元数据。构造 `ServerApp(settings, event_sink)` 可以接收事件；回调在录制分片线程上调用，接收方必须线程安全。

## 时间与边界

- 切段用媒体时间，断流检测用单调时钟，UTC 字段用于业务检索。
- 文件内时间戳从本段起点归零，索引中的 `media_start_us/media_end_us` 相对本次逐流录制。原始 RTP 时间戳不会直接拿来跨参与者比较。
- 轨道初始对齐使用上游提供的采集时间，缺少时退回接收时间。UTC 索引以第一份可录制媒体的处理时刻为锚点，不代表精确的跨客户端采集时间；本版本不提供会议多路同步播放器。
- 切点关键帧进入新片段；音频按包开始时间分段。一个 AAC 包不能无损拆开，因此音频尾部可跨越名义切点少量时间。
- 轨道发现结束后增加新轨道或改变编码参数，会结束为失败，需要开启新的流录制实例。不要将两个参与者的摄像头错误映射到同一个流标识。
- 本版本输出独立 `.mp4` 文件，不生成 HLS 的 `init.mp4/.m4s/m3u8`。不自动合成多人画面，也不添加会议成员管理、HTTP 播放接口或自动清理。
- 异常退出可能留下 `Writing` 记录和临时文件；这些记录不会进入可播放查询。本版本不自动修复文件或将残留状态标记完成，也不承诺断电后最后一段可恢复。

## 构建与验证

Linux 使用系统 FFmpeg 开发库和 SQLite（3.24 以上），不使用 `third/ffmpeg` 的预编译库：

```sh
sudo apt install build-essential cmake pkg-config python3 ffmpeg \
  libavformat-dev libavcodec-dev libavutil-dev libsqlite3-dev
cmake -S service/RecordService/Test -B build-recording-tests
cmake --build build-recording-tests -j
ctest --test-dir build-recording-tests --output-on-failure
```

测试覆盖同一会议两路独立录制、停止与重启、按媒体时间连续切段、音频先到/视频延迟时的排序、片段实时可查、数据库重开查询及失败排除。逐包比较输入输出内容与时间戳，并检查每段的 `moov/moof/mdat` 结构及 FFmpeg 完整音视频解码。样片保留在测试构建目录的 `segmented-recording-artifacts` 下。

| 测试 | 检查内容 |
| --- | --- |
| `recording_fixture` | 生成 H264/AAC 测试素材 |
| `test_recording` | 封装、音视频轨道顺序、不同 endpoint、参数变化 |
| `recording_control` | 两路独立开始、停止与重启，短录制收尾 |
| `segmented_recording` | 连续切点、逐包内容与时间戳、混合时钟回退、持久化查询，以及关键帧缺失、索引提交失败、重命名失败 |
| `test_local_file_io` | 本地文件读写与定位 |

最近一次本机验证中，录制套件 5/5、服务套件 3/3 通过；切片测试生成的 12 份独立 fMP4 全部完成解码。这是 Windows 本机的录制链路验证，未覆盖 Linux 全量服务器构建、真实网络推流或数百路压力测试。

默认 `max_streams=16` 是资源保护上限。大会议还需按实际码率、存储吞吐和并发任务数做压力测试；功能测试不代表已验证数百路容量。
