# 运行指标

`monitoring` 是独立采集模块，自动接入 `ShardedWorkerPool`、TCP 发送缓冲、UDP 发送和 TLS 缓冲。
采集已接入路径中的全部任务与发送事件，不依赖 `PACKETIA_MEDIA_LATENCY_SAMPLE_EVERY`，也不会被媒体时延日志清零。
指标属于当前进程；多节点部署需分别查询各 SFU。

## 模块边界

| 目录 / 构建目标 | 职责与依赖 |
| --- | --- |
| `monitoring/core` / `packetia_monitoring` | 时钟、直方图、队列与发送计数、注册表、结构化快照；仅依赖 C++ 标准库及线程库 |
| `monitoring/collectors` / `packetia_resource_collector` | 操作系统 CPU、内存读取与采样差值；不依赖 HTTP 或 JSON |
| `service/MonitorService` / `packetia_monitor_service` | 实现 `IService`，通过 `ThreadManager` 管理采样线程、发布只读快照及报告健康状态 |
| `server/api/MetricsController`、`MetricsJsonEncoder` / `packetia_monitoring_api` | HTTP 路由、JSON 编码；读取监控服务发布的快照 |
| `server/api` / `packetia_api` | 通用路由和有界请求调度，不链接账号服务 |
| `server/api` / `packetia_account_api` | 账号 Controller 与旧账号构造接口兼容层，依赖通用 API |

`ServerApp`、`ControlApp`、`SfuApp` 各自持有 `MetricsContext` 和监控服务。
注册表与累计数据由应用上下文持有，不再是进程永久单例。为适配现有全局 `WorkerService`
和深层 socket 调用，核心保留一个有作用域的进程绑定：应用启动时激活，停止/回滚时解除；
未激活时埋点为空操作。同一进程同时只允许一个活动应用上下文，独立注册表可单独创建与测试。
Worker 和连接只持有自身指标句柄；注册表使用弱引用，不延长业务对象寿命。
任务类型名称由 Worker 适配处注册，核心没有 `WorkType` 枚举序号表。

监控服务先于业务生产者启动，晚于业务生产者停止。采样线程每秒读取结构化快照与系统资源，
随后以 `shared_ptr<const MonitorSnapshot>` 原子发布。HTTP 查询不读取系统文件、不获取生产者指标锁。
生产者锁内只复制原始统计；忙碌率计算与 JSON 编码在锁外进行。

首次启动采样失败会导致服务启动失败，并按应用生命周期回滚；运行中采样异常保留上次快照，
通过 `monitor.healthy/error_code/snapshot_age_ms` 暴露健康状态与数据年龄。
服务未启动、停止或已销毁时，指标路由返回 503。
HTTP 仍复用已有 I/O EventLoop，本次拆分没有新增独立 HTTP I/O 线程。

## HTTP 访问

单进程 `Packetia` 复用配置中的 HTTP 监听，开启 `http.enabled` 后访问：

```sh
curl http://127.0.0.1:8081/metrics
curl http://127.0.0.1:8081/api/v1/metrics
```

两条 GET 路由返回同一 JSON 结构，禁止缓存，不需要登录。`PacketiaControl` 在公共 HTTP
监听提供相同路由。HTTP 后端需要构建时启用 `PACKETIA_WITH_LIBWEBSOCKETS`。

独立 `PacketiaSfu` 可在其进程配置中开启专用监听（省略 `metrics_http_port` 则不监听）：

```json
{
  "metrics_listen_ip": "127.0.0.1",
  "metrics_http_port": 18084
}
```

将上述字段加入现有 SFU 配置。两个本地示例节点分别使用 `18084`、`18085`，
`scripts/run_cluster_local.py` 为额外节点顺延指标端口。

## 字段与口径

| JSON 字段 | 含义 |
| --- | --- |
| `workers[].pool/index` | 池名、池内 Worker 编号 |
| `workers[].queue_wait` | 投递开始到开始执行的耗时，包含入队锁等待；`count/mean_us/max_us/p50_us/p95_us/p99_us` |
| `workers[].execution` | 每个任务的处理耗时，包含同轮 tick，异常任务也计入 |
| `workers[].task_types` | 按 `rtp/rtcp/stun/dtls/raw_tcp/raw_udp/timer/control/function/recording` 分组的等待和执行耗时 |
| `workers[].queue_depth/queue_bytes` | 等待任务数、payload 实际占用的存储字节数 |
| `workers[].oldest_job_wait_ms` | 队列中最老任务已等待多久 |
| `workers[].active/active_task_ms` | 是否正在处理任务、当前任务持续时间；卡死任务完成前也能看到 |
| `workers[].busy_percent` | 近 5 秒任务忙碌时间占比，包含当前尚未结束的任务；包含锁等待，不等于线程 CPU 使用率 |
| `workers[].dropped_total` | 超限或不排空关闭时丢弃的任务数 |
| `send_queue.bytes/packets/oldest_wait_ms` | 当前发送缓冲合计字节数、缓冲项数、最老项等待时间；空队列等待为 0 |
| `send_queues[]` | 非空发送队列明细，含进程内唯一 `id`、`tcp/tls_plaintext/tls_ciphertext` 阶段 |
| `send.tcp/udp.bytes_total/packets_total` | socket 已接受的累计字节数、已发送包数 |
| `send.tcp/udp.bitrate_bps/packet_rate_pps` | 近 5 秒字节/包数计算的 bit/s、包/s，连续采集，不依赖查询频率 |
| `send.tcp/udp.failures_total` | 发送失败次数；UDP 无重试队列，暂不可写也计失败 |
| `send.tcp/udp.blocked_total` | 暂不可写次数；TCP 的 EAGAIN 不算永久失败，EINTR 重试不重复计数 |
| `send.tcp/udp.rejected_total` | 缓冲容量/分配不足拒绝的次数 |
| `send.tcp/udp.abandoned_packets_total` | 关闭时未发完的 TCP 缓冲项数 |
| `resources.process_cpu_percent` | 进程 CPU；占满一个逻辑核为 100%，可以超过 100% |
| `resources.host_cpu_percent` | 整机 CPU 忙碌率，0–100% |
| `resources.process_cpu_seconds/process_rss_bytes` | 进程累计 CPU 秒、驻留内存字节数 |
| `resources.host_memory_total_bytes/host_memory_available_bytes/logical_cpus` | 整机物理内存、可用内存、逻辑核数 |

耗时分布是当前 Worker 实例生命周期累计值；p95/p99 为固定直方图桶上界近似值，每次翻倍细分 8 桶。
Worker 或连接销毁后对应明细自动移除，发送累计计数保留到进程退出。
码率、包速率和忙碌率使用固定秒桶，近 5 秒窗口具有最多一秒的边界粒度，启动不足 5 秒时仍按 5 秒计算。
TCP 的包数指完整写完的应用缓冲项，不代表网卡 TCP 分段数；部分写入立即计字节，只在写完时计一个包。
实际发送指内核 socket 接受，不代表对端收到；不含 IP/UDP/TCP 链路头。
TLS 的明文、BIO 密文和 TCP 队列是不同阶段，字节合计用于观察缓冲占用，不能当作未发送的媒体净荷大小。
指标不覆盖 libwebsockets 自己的 HTTP/WebSocket 发送队列或内核 socket 队列。

HTTP 默认返回最近一秒内发布的快照，任务年龄与速率均以快照时间计算。
CPU 使用率首次采样为 `null`，后台采样至少间隔 1 秒后提供差值。不支持/读取失败的资源项返回 `null`。
Worker 忙碌率记录任务期间的墙钟时间，空闲时的 tick 不计入该值。
服务默认监听本机，若修改为外部地址，指标随该监听开放。

## 验证

```sh
cmake -S monitoring/test -B build/monitoring-tests
cmake --build build/monitoring-tests --parallel
ctest --test-dir build/monitoring-tests --output-on-failure
```

独立测试不依赖 FFmpeg、GoogleTest 或 HTTP 后端，覆盖直方图、未完成任务、队列年龄、并发采集、应用隔离、
服务启停/重启、后台采样、快照缓存，以及不链接账号库的 HTTP 请求调度与启动失败回滚。
TCP 部分写入、EAGAIN 和关闭丢弃由 `utils/test/latency` 的脚本化 socket 测试覆盖。

目前未统一接入 AI、混流、转码自有线程的队列；本轮仅调整模块职责与依赖，保留既有采集范围。
