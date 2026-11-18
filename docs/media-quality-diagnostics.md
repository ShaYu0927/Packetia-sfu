# 媒体质量诊断日志

用于把“网络反馈 → 算法决策 → 发送队列 → 视频状态”关联起来。
日志是诊断数据，不会自动学习参数，也不会替代真实媒体路径的弱网测试。

## 开启与查看

原生服务默认关闭媒体诊断，启动前设置：

```sh
export PACKETIA_QOS_LOG=1
export PACKETIA_LOG_FORMAT=json
export PACKETIA_LOG_LEVEL=info
export PACKETIA_LOG_MODE=console
```

`PACKETIA_QOS_LOG` 支持 `1` / `true`，首次检查后固定，修改需要重启。
INFO 级别以上的过滤会禁用这些事件。启用后，现有有界异步日志队列负责输出和文件轮转。
生产环境使用 `console` 时由进程管理器或 Docker 收集；使用 `file` / `both` 时，
通过 `PACKETIA_LOG_FILE` 指定日志文件，默认 16 MiB 轮转并保留三份备份。

Docker 集群在 `runtime/.env` 中设置 `PACKETIA_QOS_LOG=1`，重新创建容器后查看：

```sh
python3 deploy/cluster/deploy.py up --recreate
```

具体启动命令及 Compose 文件选择见[部署说明](../deploy/cluster/README.md)。
浏览器示例默认开启 JSON 与文件/控制台输出，位置是
`examples/webrtc-room/artifacts/server-qos.jsonl`；原生服务的默认日志配置不变。

在 `examples/webrtc-room` 运行：

```sh
npm run qos:summary
```

生成 `artifacts/server-qos-summary.json`。
指定其他 JSON 日志或仅查看某个房间：

```sh
PACKETIA_QOS_TRACE_FILE=/path/to/server.jsonl PACKETIA_QOS_ROOM=demo npm run qos:summary
```

汇总器读取当前文件；轮转后查历史需保留并处理对应备份。汇总结果不是所有历史流量的完整账本。

## 关联与事件

JSON 顶层保留 `timestamp`、`service`、`node_id`、`pid`、`tid`、`event`。
业务字段在 `data` 中，数字、布尔值、字符串与 `null` 保留类型。

以 `node_id + pid + transport_id` 识别下行连接；进程重启或换节点后不能仅按数字 ID 合并。
当前 WebRTC 的 `transport_id` 与信令返回的媒体 `session_id`、浏览器报告的 `sessionId` 一致。
`qos_session` 记录房间、参会者和 publish/subscribe 角色；
`qos_subscription` 记录订阅 ID、源轨道 ID、源/目的端点、输出 SSRC 和 TWCC 扩展 ID。
轨道事件用同一连接的输出 SSRC 关联到订阅。

| 事件 | 触发时机与内容 |
| --- | --- |
| `qos_session` | 媒体会话建立，提供房间/成员映射 |
| `qos_subscription` | 下行订阅建立，提供轨道和 SSRC 映射 |
| `qos_sample` | 每个下行连接约每秒一次，包含反馈、目标、发送完成速率和队列 |
| `qos_allocation` | 同一采样周期每轨一次，包含需求、优先级、准入结果和预算 |
| `qos_control_state` | Worker 观察到路径、网络可用性、反馈有效性、反馈来源或时延使用状态变化 |
| `qos_track_state` | 暂停、等待关键帧、恢复转发或恢复发送失败 |
| `qos_probe_decision` | 候选探测原因变化，包括冷却、拥塞和预算不足 |
| `qos_probe_started` | 探测簇被接受，记录额外 Padding 速率、总 Pacer 速率、时长和字节上限 |
| `qos_probe_execution` | 执行结束或撤销；`accounted_bytes` 是入队预算计费，不是实际收到量 |
| `qos_probe_result` | Worker 首次观察到一个簇的结果，包括成功容量或失败类别 |

采样由现有下行 Worker Tick 驱动，不增加线程或逐包日志；没有订阅时不产生周期样本。
Tick 延迟也会延迟采样，短于 Tick 的状态变化不保证全部被记录。
探测结果按代次和簇编号去重，控制状态不变时不重复输出事件。
关闭诊断时保留发送/反馈计数，不构建日志字段。

## 字段口径

| 字段 | 含义 |
| --- | --- |
| `monotonic_ms` / `path_epoch` | 本机稳态时钟与网络路径代次 |
| `feedback_valid` / `feedback_age_ms` | 是否有未超时的控制输入，以及距该输入的本机时间 |
| `feedback_source` | `none`、`rr` 或 `twcc`，表示当前控制输入来源 |
| `twcc_age_ms` | 距最近匹配成功的 TWCC 的时间；没有时为 `null` |
| `bwe_estimate_valid` | TWCC 接收速率估计是否初始化，区别于“已经收到某种反馈” |
| `usage` | BWE 报告的 normal / underusing / overusing；RR 路径为 `null` |
| `delay_trend_ms` / `delay_slope` | 分组观察器实际提供的趋势与回归斜率（ms/ms）；未提供时为 `null` |
| `loss_rate` | 最近控制输入中的丢包比例，不是生命周期累计丢包率 |
| `receive_bitrate_bps` | 初始化后的 TWCC 接收速率估计，不是链路物理容量 |
| `smoothed_rtt_ms` / `smoothed_jitter_ms` | 现有统计器的 EWMA 结果；尚未测到或过期为 `null` |
| `target_bitrate_bps` | 策略输出目标，不等于实测可用带宽 |
| `normal_pacing_bps` | 常规策略给出的 Pacer 速率 |
| `pacing_bitrate_bps` | 采样时 Pacer 使用的速率，包含可能生效的探测提速 |
| `sent_rtp_bps` / `sent_packets_per_second` | 相邻样本实际发送完成计数之差除以实际时间差 |
| `sent_probe_rtp_bps` | 上述发送速率中属于探测簇的 RTP 部分 |
| `*_total` | 当前控制器生命周期累计计数，路径切换不清零 |
| `unmatched_twcc_packets_total` | 没匹配到发送历史的回执项数量；可能来自重复回执、过期或缺失历史，不是网络丢包数 |
| `queued_bytes` / `queued_packets` | Pacer 容量占用，包括正在消费但尚未返回的包 |
| `oldest_wait_ms` | 采样时最老排队包等待时间，包含控制包 |
| `estimated_drain_ms` | 按当前速率估算队列排空时间，不是端到端时延 |
| `rejected_packets_total` | Pacer 累计拒绝包数，包括容量、无效输入和内存分配失败 |
| `budget_estimate_valid` | 分配预算自身的估计有效标志；分配器还会检查预算时间和路径 |
| `required_bps` / `committed_bps` | 源需求及本次提交的发送占用，恢复余量不算实际占用 |

WebRTC 的发送完成边界是 UDP socket 接受数据，不代表网卡出线或接收端收到。
其他 Transport 仍遵循各自的完成回调语义，可能只是入队，不能一律解释为 socket 发送。
未协商 TWCC 时也记录实际完成计数，但不会虚构 TWCC 序号或发送历史。
发送 RTP 字节包含 RTP 头、音频、重传和探测，不含 UDP/IP/SRTP 附加开销；
浏览器面板的视频码率统计口径不同，不能要求二者逐采样相等。

首个窗口、跨路径窗口、计数回退或时钟倒退时，区间速率为 `null`。
没有可读 Pacer 的协议，其队列字段为 `null`。
队列是每秒的瞬时快照，不代表该秒内的最大等待或所有包的 P95。
`monotonic_ms` 只能在同一服务器进程中比较，不能直接减去浏览器或其他机器的时间来计算单向时延。
诊断不记录媒体内容、JWT、TURN 密码、ICE 凭据或 SDP。

## 下行 TWCC 协商

`RoomSignalHandler::HandleSubscribe` 为订阅端独立配置 MID、TWCC 头扩展与 `transport-cc`
反馈能力，不依赖发布端是否支持 TWCC。SDP 协商器与浏览器 Offer 取交集，Answer 使用
浏览器提供的扩展 ID，且仅支持现有发送器可写入的 1–14 范围。

创建下行发送器时，从 Answer 提取 `transport_cc_extension_id`，只有扩展和反馈能力同时
协商成功才启用。`RtpSenderTrack` 使用该下行连接共享的序号分配器，给实际发出的 RTP
写入编号，并通过发送完成回调登记历史；浏览器的 TWCC 经 `WebRtcMediaTransport`、
`SendSideController` 匹配历史后交给分组趋势和带宽控制器。

不支持 TWCC 的浏览器仍可使用 RR。发布端当前不声明接收侧 TWCC 能力，这次接通的是
“服务器 → 订阅浏览器”方向，不能由此推断上行也已实现 TWCC 接收反馈生成。

## 如何用于优化

固定视频素材和输入负载，再在实际媒体 UDP 路径施加容量下降、排队、丢包和恢复：

1. 看 `feedback_source` 和计数，先确认所研究的算法确实拿到输入。
2. 看趋势、丢包与目标，判断是否及时减小预算、是否反复振荡。
3. 看队列、拒绝包与轨道状态，判断控制动作有没有改善发送压力。
4. 看探测事件，判断是否冷却过久、回执不足、过载撤销或成功后仍未达到恢复门槛。
5. 对照同一会话的浏览器报告，比较接收帧率、冻结和恢复耗时。

例如，实际吞吐 260 kbps、目标 2.5 Mbps，并不说明已经验证了 2.5 Mbps 容量；
没有足够输入负载时，应先增加实际视频流量，而不是据此调大恢复或探测参数。

## 本次验证与发现（2026-11-18）

- 结构化日志与四组传输/算法 CTest：5/5 组通过。
- 新增测试覆盖 typed JSON/转义/空值、采样限频、路径与时钟变化、发送完成计数、
  重复 TWCC、过期只读快照及探测事件去重。
- 单独启用诊断运行 15 个下行分配测试，确认暂停、等待关键帧、恢复、探测开始和执行结束日志实际输出。
- 浏览器测试使用 `PACKETIA_EXPECT_QOS_LOG=1`，确认能按本次房间关联实际发送速率、RTCP 反馈与分配事件。
- 初次日志检查发现浏览器示例仅有 RR、TWCC 为 0：下行继承发布端的 MID-only 能力，
  且未给发送器配置 TWCC 扩展。现已补上独立下行协商与发送器配置。
- 新增 `webrtc_twcc` 信令集成测试覆盖发布端没有 TWCC、订阅端扩展 ID 不同、
  仅反馈、仅扩展和两者均缺失的情况。
- 补齐协商后，6/6 组信令 CTest 与 `PACKETIA_EXPECT_TWCC=1` 浏览器质量测试通过。
  首次订阅的最后周期样本记录 320 份 TWCC、1653 个匹配包、0 个未匹配回执项；
  重新订阅记录 96 份 TWCC、662 个匹配包、0 个未匹配回执项。两条连接均出现实际回归斜率
  与有效分配预算；720p60 阶段接收解码约 60 fps，ICE restart 后继续解码。
  这些是本机回环链路结果，尚未验证受控弱网下的拥塞响应与恢复探测效果。

运行入口：

```sh
PACKETIA_LOG_MODE=console ctest --test-dir build/webrtc-integration --output-on-failure \
  -R '^(StructuredLoggerTest|webrtc_trendline|media_pacer|test_weak_network_manual|test_rtp_input)$'
cd examples/webrtc-room
npm test
PACKETIA_EXPECT_QOS_LOG=1 npm run check:quality
PACKETIA_EXPECT_TWCC=1 npm run check:quality
npm run qos:summary
```

浏览器测试前需保持示例服务运行；非默认端口需同时设置 `PACKETIA_DEMO_URL`。
