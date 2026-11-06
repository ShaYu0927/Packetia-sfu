# 技术参考

项目概览、当前构建方式与模块边界见[根 README](../README.md)。
以下保留协议、线程、控制面和媒体处理的详细说明；其中的验证记录对应记录时的环境与版本。
除非特别说明，命令均从仓库根目录执行。

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

各路独立 fMP4 切片、SQLite 索引及查询接口见 [RecordingService](../service/RecordService/RecordingService.h)
和 [RecordingCatalog](../service/RecordService/RecordingCatalog.h)。
按 `(session_id, stream_id)` 分别录制，在关键帧处按媒体时间切段；支持逐流停止、重启，以及按会话、流和时间范围查询完成片段。构建需要 SQLite 3.24 以上开发库。

## 录像画面合成实验

可通过[双人合成示例](../service/RecordService/examples/README.md)体验两路视频左右拼接和音频混音，
支持生成测试画面或输入两份已完成的录像，输出单路 H264/AAC MP4。

## 会议合成框架

[ConferenceMixService](../service/ConferenceMixService/README.md) 提供会议输入配置、独立工作线程、任务启停与编码输出接口。
当前尚未实现实际拼画面、混音和编解码。服务已注册到统一启停管理，默认关闭。

录制、AI、会议合成支持全局开关、流默认值与逐流覆盖配置，字段见
[AppConfig](../config/AppConfig.h) 和 [配置示例](../config/packetia.example.json)，统一启停入口见
[ServerApp](../server/ServerApp.cpp)。

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

线程创建统一经过 [`utils::ThreadManager`](../utils/ThreadManager.h)，由 `ManagedThread` 持有线程、
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
HTTPS 证书配置、CORS 和性能压测尚未完成。实现入口见 [HttpServer](../protocol/http/HttpServer.h)，
服务接口见 [IHttpServer](../protocol/http/IHttpServer.h)。

API 采用 ZLMediaKit `WebApi.cpp` 的路由注册、参数适配和统一回复思路。
[ApiRouter](../server/api/ApiRouter.h) 按 HTTP 方法与路径精确匹配；各业务模块通过
`RegisterRoutes` 注册独立处理函数，再由 `ApiServer` 在启动时构建只读路由快照。
新增接口在所属模块注册。`ApiServer` 支持注入已构建的路由，`ControlApp` 装配账号与房间路由；
原账号构造方式仍由 `ApiServer.cpp` 的 `BuildRoutes` 装配。
当前账号注册入口见 [AccountController](../server/api/AccountController.cpp)：

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
[`AccountController::RegisterRoutes`](../server/api/AccountController.cpp)。
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
完整字段口径、SFU 配置示例和独立测试命令见 [monitoring 模块文档](../monitoring/README.md)。

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
| [Rtp.h](../Rtsp/Rtp.h)、[Rtp.cpp](../Rtsp/Rtp.cpp) | 固定头与接收统计；完整 SSRC 碰撞管理和 RTCP 调度仍需核对 |
| [RtpReceiver.cpp](../Rtsp/RtpReceiver.cpp) | 版本/PT/SSRC 校验，按 CC/X/P 计算载荷位置 |
| [RtpHeaderExtensions.h](../Rtsp/Rtp/RtpHeaderExtensions.h) | 单/双字节解析，未知 profile 跳过；重复 ID 按本地策略拒绝；ID=0 且 len 非零的停止规则待补，失败可能留下部分输出 |
| [RtpTransportCcExtension.cpp](../Rtsp/RtpTransportCcExtension.cpp) | 仅单字节 TWCC、ID 1..14、两字节序号 |
| [RtpSenderTrack.cpp](../Rtsp/RtpSenderTrack.cpp) | 下游改写、缓存、NACK 原包重发；MID/TWCC 输出为单字节，尚非完整 RTX |
| [H264RtpPayloadParser.cpp](../Common/Depacketizer/H264RtpPayloadParser.cpp) | Single NAL/STAP-A/FU-A，不代表全部 H264 打包模式 |
| [AudioRtpDepacketizerFactory.cpp](../Common/Depacketizer/AudioRtpDepacketizerFactory.cpp) | G.711/Opus 接收工厂，其他路径/编码需单独核对 |
| [RtcpReciver.cpp](../protocol/Rtcp/RtcpReciver.cpp) | SR/RR/BYE、NACK/PLI/FIR、传输反馈入口；当前跳过 SDES，枚举不代表处理器已实现 |
| [SdpNegotiator.cpp](../src/sdp/SdpNegotiator.cpp) | PT/codec、方向、BUNDLE、扩展协商；WebRTC 使用 UDP DTLS-SRTP 与 rtcp-mux |

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

以下为原有示例的运行记录。当前源码快照未包含 `server/examples/WebRtcRoomDemo.cpp`，
因此不会生成 `PacketiaRoomDemo` 目标；使用这些步骤需要该入口或已有的兼容二进制。

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

音频等通用接收路径使用 [RtpPacketReorderQueue](../Rtsp/RtpPacketReorderQueue.h)，按展开后的序号排序，
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
网络断开或显式路由变化事件会清空旧估计；反馈中断时由下面的周期处理撤销过期状态。

SRTP 发送端允许重新保护缓存的原 RTP 序号及 payload，重传重新分配 TWCC 序号；
接收端继续执行认证和防重放。当前协商的保护配置为 `SRTP_AES128_CM_SHA1_80`。

视频 NACK 在乱序等待后发起，按 RTT 重试；最后一次请求仍保留响应时间，超过恢复期限则
回退到 PLI。下行重传默认限制原包年龄为 500 ms，发布轨道合并不同订阅者的 PLI 请求。
WebRTC 会话的 100 ms 定时器向媒体池投递恢复任务，停止收包后仍能推进重试与超时。

业务可通过 `SetUpdateCallback` 消费控制输出。订阅管理器使用独立观察者获取预算，不覆盖业务回调；
通知合并后回到订阅端媒体 Worker 读取最新状态，周期恢复任务继续推进分配和超时。

### 反馈超时与重新估算

`DownlinkTrackManager::Tick` 按 Transport 驱动 `SendSideController::OnProcessInterval`，
复用 WebRTC 会话现有的约 100 ms 恢复调度，停收 RTP/RTCP 后也能检查反馈期限。
独立使用发送控制器的调用方需要以本地单调时钟周期调用 `OnProcessInterval(now_ms)`。

`NetworkControllerConfig::feedback_timeout_ms` 默认 5000 ms，必须大于 0。
连续达到该期限没有有效 TWCC 或 RR 时，清空旧目标码率、Pacer 建议、质量状态及估算历史，
向业务回调发送一次空 `NetworkControlUpdate`，并通知预算观察者；后续空闲 Tick 不重复通知或重放关键帧请求。
RTT-only 更新、重复或无法匹配发送历史的 TWCC、未知发送 SSRC 的 RR 不延长期限。
接收反馈时也先检查超时，避免定时任务延迟时继续沿用旧估计。

超时不改变连接可用状态、路径代次、发送器登记或 RTP/TWCC 序号；已有订阅保持当前转发或暂停状态。
发送历史只移除达到超时年龄的包，保留近期在途包，让恢复的反馈仍能匹配；过旧包的迟到 TWCC 无法重建估计。
新反馈从配置的初始码率重新估算，带宽尚未初始化时保持原分配。
即使 RR 持续到达，长期缺失的 TWCC 估算历史也会重置，防止恢复时跨越静默区间复用旧时延和速率窗口。

自动订阅分配的 TWCC 新鲜度仍为 1500 ms：超过后先冻结分配；上述 5000 ms 期限用于撤销控制器旧状态，
并不增加首次订阅等待时间，也不触发 ICE 断开、自动退订或周期接收质量评估。

### 分组观测与主动探测算法扩展框架

框架已接通，新增核心算法尚未实现。默认继续使用现有 OU 逐包估算和受限 padding 恢复探测。
每条下行 Transport 独占观测器及探测控制器，同一连接的音视频共享它们，不为每条轨道重复估算。

算法填写入口：

| 文件与方法 | 输入 | 留给算法实现的内容 |
| --- | --- | --- |
| `media/quality/bwe/PacketGroupObserver.cpp`：`OnFeedback` | 已匹配发送历史的 TWCC、目标码率、RTT | 分组边界、跨反馈组装、去重、有界窗口、跨组趋势及拥塞判断 |
| `media/quality/ProbeController.cpp`：`OnProcessInterval` | 活动媒体需求、候选视频需求、码率约束、当前估计及 RTT | 是否探测、退避、额外 padding 码率、时长和字节预算 |
| `ProbeController::OnFeedback` | TWCC、完成的观测组及本轮 BWE | 按 `probe_cluster_id` 收集反馈并评估恢复空间 |
| `ProbeController::OnProbeEvent` / 两个组件的 `Reset` | 执行起止、中止及重置 | 维护探测状态，清理历史与未完成结果 |

`PacketGroupObservation` 的趋势、拥塞状态、接收速率、丢包率均为可选输出；某项未给出时使用对应的原有逐包结果。
分组丢包率参与带宽估算，业务质量统计仍保留原始反馈丢包率。发送/反馈时间使用本机单调时钟，
接收时间使用对端时钟；应比较各自时钟内的间隔，不能直接相减当作单向时延。

`ProbeDecision` 区分三种情况：默认 `{}` 沿用原有探测策略；`{true, std::nullopt}` 接管策略但本轮不探测；
`{true, ProbePlan{bps, duration_ms, max_bytes}}` 请求探测，三个参数均为活动媒体之外的额外 padding 预算。
当前周期决策仅在存在可投递、协商 TWCC 的带宽暂停视频且没有执行中的探测时调用；仍使用现有媒体 Worker Tick。

框架负责选择承载轨道、限制额外码率不超过最大码率扣除活动媒体需求后的余量、
限制单簇至 500 ms / 128 KiB、限制单次 Tick 约 16 KiB，并分配不重复的簇编号。
`RtpSenderTrack::SendPadding` 将簇编号及 `MediaKind::Padding` 存入发送历史，TWCC 匹配后回到算法。
`Started`、`Finished`、`Aborted` 表示执行状态，`Finished` 不代表已经测出更高容量；容量判断由算法依据反馈完成。
执行事件的字节数使用每包 300 字节的保守计费，反馈中保留实际 RTP 大小。

删去承载订阅时中止执行；切网、断网及反馈超时时重置或重建算法，并以路径代次和探测代次过滤旧执行事件。
重置后仍能匹配的近期在途反馈保留原簇编号，算法只应将自己已登记的簇计入探测结果。
算法方法在 `SendSideController` 锁内串行运行，只做有界计算，不允许发包、IO、阻塞或回调外层控制器。

可以直接补上述默认实现；也可通过工厂替换接口，每次工厂调用必须产生独立实例：

```cpp
media::NetworkControllerConfig config;
config.packet_group_observer_factory = [] { return std::make_unique<MyPacketGroupObserver>(); };
config.probe_controller_factory = [] { return std::make_unique<MyProbeController>(); };
auto controller = std::make_shared<media::SendSideController>(config);
// MyPacketGroupObserver / MyProbeController 分别实现 IPacketGroupObserver / IProbeController。
```

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
默认策略每 3 秒最多一个 500 ms 探测簇，每簇计费上限 128 KiB，单次 Tick 突发上限约 16 KiB。
探测总目标受控制器最大码率及当前目标的增长上限约束，并扣除活动媒体需求；padding 使用新 RTP/TWCC 序号，
不进入 NACK 缓存。用户暂停或源暂停期间不发送探测；最后一条订阅移除时注销对应观察者。

当前执行的是单编码视频的整轨暂停/恢复，尚未接入 simulcast/SVC 选层、编码器动态调码率或实际 Pacer；
受限 padding 探测也不等同于完整的探测与 pacing 系统。策略接口目前仅提供 C++ API，未新增信令字段。
组件及 Worker 集成测试不代表真实浏览器弱网质量已经验收。

## 本轮验证

2026-11-05 分组观测与探测框架验证（macOS）：新增 7 项测试，覆盖空算法回退、分组结果传递、
非有限结果回退、自定义探测接管、执行限幅、真实 padding/TWCC 簇编号关联、执行完成与退订中止、
反馈超时重置及 Transport 隔离。`Packetia` 及相关测试目标构建通过，六组相关 CTest 回归全部通过，
`git diff --check` 通过；新分组和探测策略仍为 TODO，本轮未验证新算法的效果。

2026-11-05 发送反馈超时验证（macOS）：新增 8 项测试，覆盖超时撤销与重新初始化、RTT-only 不延长期限、
RR 回退期间的 TWCC 历史重置、时间零值和自定义期限、迟到与重复反馈过滤、近期在途包保留、
通知去重、Transport 隔离、回调内退订及实际媒体 Worker 上停包后的周期处理。
`Packetia` 及相关测试目标构建通过，`test_weak_network_manual`、`test_rtp_input`、`test_udp_binding`、
`media_core`、`test_webrtc_crypto`、`webrtc_service` 共 6 组 CTest 全部通过；本轮未进行真实浏览器弱网验证。

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
