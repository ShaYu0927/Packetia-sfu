# RTP 转发耗时

转发链路默认开启耗时统计，每个线程约每 5 秒输出一行 `[media-latency]` INFO 日志。
使用 `steady_clock` 的纳秒时间戳，日志统一显示微秒（`us`，1000 us = 1 ms）。
直方图和计数器按线程保存，逐包记录不分配内存、不获取统计全局锁；日志在 Worker/IO
循环中汇总输出，不在 socket 或任务队列锁内格式化。Worker/IO 循环正常退出时输出剩余窗口。

## 如何观察

```sh
PACKETIA_MEDIA_LATENCY_SAMPLE_EVERY=1 \
PACKETIA_MEDIA_LATENCY_INTERVAL_MS=5000 \
./build/Packetia
```

建立实际订阅转发后，查看运行目录下的日志：

```sh
rg '\[media-latency\]' app.log
```

优先看 `udp_total_us` / `tcp_total_us` 的 `avg`、`p95_le`、`p99_le`、`max`。
例如 `p99_le:800` 表示这个线程、本次窗口的成功发送样本中，至少 99% 的耗时不超过
800 微秒。这是解释字段的假设数字，不是项目实测结果。

如果日志出现 `no_subscribers`，却没有 `udp_sent` / `tcp_sent` 和相应的 `total`，
说明这些输入没有实际下游订阅者；需要先建立 Sender 与 Transport 的订阅关系。
没有成功发送时不会输出虚假的 0 微秒转发延迟。

## 统计口径

统计单位是 **RTP 包**，不是完整视频帧。一次输入转发给 N 个订阅者，最多产生 N 个
成功发送样本；`fanout` 每个输入包只记一次。一帧可以包含多个 RTP 包，不能把包延迟
相加当成一帧延迟。

计时起点是 `MediaEndpointIngress::OnMediaPacket` 接收到有效、完整的 RTP 包。
此前的内核收包等待、TCP 缓冲及拆包、解密、接入层复制不在统计范围内；也不包括
操作系统发送缓冲、网卡排队、网络传输、接收端解码和播放时间。

| 日志字段 | 起点 → 终点 | 样本单位 |
| --- | --- | --- |
| `ingress_us` | 媒体入口 → 调用分片队列 post | 每个已出队的采样输入 |
| `worker_queue_us` | 调用分片队列 post → Worker 出队后开始执行 | 每个已出队的采样输入，含入队锁等待 |
| `before_forward_us` | Worker 开始执行 → 进入订阅转发函数 | 每个走到转发函数的采样输入，含接收校验、组帧及同步帧发布 |
| `fanout_us` | 进入订阅转发函数 → 全部 Sender 调用返回 | 有订阅者的采样输入，含路由查询、改写、发送调用、重传缓存维护 |
| `udp_send_us` | Sender 调用 Transport 发送 → UDP sendto 成功返回 | 每个成功 UDP 下游发送，含 Transport 处理、跨线程调度和系统调用 |
| `udp_total_us` | 媒体入口 → UDP sendto 成功返回 | 每个成功 UDP 下游发送 |
| `tcp_send_us` | Sender 调用 Transport 发送 → 整个 interleaved 包写入 socket | 每个成功 TCP 下游发送，含封装、IO 调度、写队列等待、部分写重试 |
| `tcp_total_us` | 媒体入口 → 整个 interleaved 包写入 socket | 每个成功 TCP 下游发送 |

UDP 的 `total` 在 IO 线程记录，不包含成功发送后 Worker 被唤醒的时间。
TCP 在 `BufferWirte` 中最后一个字节被 `send()` 接受后才记录一次；入队、部分写入、
EAGAIN 都不算发送完成。发送失败和连接关闭丢弃不进入成功耗时分布。
RTCP、NACK 重传、普通 RTSP 控制消息不会混入 RTP 首次转发统计。

`fanout` 对 TCP 测到的是所有发送任务提交完毕，对 UDP 则包含同步等待及返回后的工作；
它与各订阅者的 `total` 是不同指标。各指标的样本数、线程和阶段存在差别，不能把
P99 相加来推算总 P99，也不能直接平均不同线程的 P99。

每个耗时字段包含：

- `n`：本窗口实际样本数。
- `avg`、`max`：样本耗时的平均值、最大值。
- `p50_le`、`p95_le`、`p99_le`：直方图近似分位数的上界，单位为整数微秒。
  16 us 以内桶宽为 1 us，之后每次翻倍分 8 桶；超过约 4.77 小时进入溢出桶。

同时输出非零计数器：`ingress_sampled`、`worker_started`、`queue_dropped`、
`forward_inputs`、`no_subscribers`、`sender_attempts`、`sender_rejected`、
`udp_sent`、`udp_failed`、`tcp_sent`、`tcp_abandoned`。
这些都是采样包相关计数，不是全量网络丢包率；失败指标可能描述同一包的不同阶段。
例如 `sender_rejected` 表示 Sender 返回失败，`tcp_abandoned` 表示已入写队列但未写完
就被销毁的包。输入检查拒绝、端点不存在等也会使输入计数与成功发送数不同。
多订阅者场景下一份输入会对应多份发送，窗口边界和跨线程交接也会导致计数不相等。

## 控制开销

- `PACKETIA_MEDIA_LATENCY_SAMPLE_EVERY=1`：默认全量采样。
- 设置为 `16`：每个入口线程每 16 个有效 RTP 输入采样一次，同一输入的所有订阅者
  沿用这个选择。周期采样可能遗漏周期性尖峰，调查关键帧时建议设为 1。
- 设置为 `0`：关闭采样及汇总日志。
- `PACKETIA_MEDIA_LATENCY_INTERVAL_MS=5000`：默认汇总周期，可修改，启动时读取一次。

采样间隔接受 0–1000000；汇总周期接受 0–3600000 ms，0 按 1 ms 处理。
非法值使用默认值。降低采样率后，窗口内样本太少时 P99 的参考价值也会下降。
日志输出、时钟读取、计数本身仍有成本，压测时可以用关闭统计的同条件运行作对照。

## 代码和验证

- `utils/MediaLatency.{h,cpp}`：时钟、上下文、定长直方图、线程独立汇总。
- `media/transport/MediaEndpointIngress.cpp`：选取样本，携带到 `WorkJob`。
- `utils/ShardedWorkerPool.cpp`：入队、出队时间和采样任务丢弃计数。
- `media/endpoint/MediaEndpoint.cpp`：转发前、fanout、订阅及 Sender 返回结果。
- `Rtsp/RtpSenderTrack.cpp`：只在首次 RTP 发送调用期间建立发送计时上下文。
- `network/UdpServer.cpp`：把计时上下文按值带入 IO 回调，在实际发送后记录。
- `network/BufferWrite.cpp`：把计时上下文存入 TCP 队列，写完/丢弃时记录。

TLS 上下文只覆盖当前同步调用栈；所有已有异步边界都复制时间戳。
新增 Transport 如果引入自己的异步发送队列，需要同样携带 `SendTrace` 并在真实发送
完成时调用 `SocketSent`，不能依赖另一个线程的 TLS。

可独立构建基础测试，无需 libhv / FFmpeg / GTest：

```sh
cmake -S utils/test/latency -B build-latency -DCMAKE_BUILD_TYPE=Debug
cmake --build build-latency -j
ctest --test-dir build-latency --output-on-failure
```

覆盖直方图、时间边界、采样/关闭、跨线程传递、上下文退出、真实 Worker 入队和
队列拒绝。Linux 默认额外构建 TCP 写队列测试，用可控的 send 返回值覆盖部分写入、
EAGAIN、完成只记一次、关闭丢弃和重传排除。
