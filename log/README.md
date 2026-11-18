# 日志模块

应用线程完成消息格式化后，把记录放入有界队列；独立日志线程批量写入控制台或文件。
日志队列继续使用互斥锁保护，业务线程不等待磁盘 I/O；日志字符串格式化和短暂入队锁仍有成本。

| 文件 | 职责 |
| --- | --- |
| `LoggerConfig.h` | 输出方式、格式、级别和环境变量解析 |
| `LogRecord.h` | 文本/JSON 格式化、UTC 时间戳、字段转义 |
| `logger.h` | 异步队列、文件轮转、统计、全局实例与 `LOG_*` 宏 |

## 初始化与配置

可执行程序在创建业务服务和线程之前调用一次：

```cpp
InitializeLogger("sfu", "sfu-a");
LOG_INFO("Node registered");
LOG_ERROR("Node registration failed, status=", status);
```

Control 使用 `service=control`，SFU 使用 `service=sfu`，并从配置文件读取 `node_id`。
单进程入口使用 `packetia`，浏览器演示入口使用 `room-demo`。
进程身份在初始化后保持不变，重复初始化相同身份允许，切换身份会抛出异常。
未显式初始化的工具和测试首次使用日志时采用 `service=packetia`。

| 环境变量 | 可选值 | 原生程序默认值 |
| --- | --- | --- |
| `PACKETIA_LOG_MODE` | `console`、`file`、`both` | `both` |
| `PACKETIA_LOG_FORMAT` | `text`、`json` | `text` |
| `PACKETIA_LOG_LEVEL` | `debug`、`info`、`error` | `info` |
| `PACKETIA_LOG_FILE` | 日志文件路径 | `app.log` |

环境变量只在全局日志实例首次初始化时读取，修改后需重启进程。
Docker Compose 固定使用 `console`，默认格式为 `json`，级别为 `info`；
原生本地集群启动器固定使用 `console`，由启动器保存并轮转每个进程的输出，
确保设置了文件日志环境变量时，启动器仍能收到就绪事件。

`LOG_DEBUG`、`LOG_INFO`、`LOG_ERROR` 保持现有调用方式。
运行时禁用的级别不会求值宏参数，因此不要在日志参数里写业务必需的副作用。
默认编译保留 DEBUG，运行时默认关闭；若构建时提高 `LOGGER_MIN_LEVEL`，
被编译移除的级别无法通过环境变量恢复。

## 日志记录

```json
{"timestamp":"2026-11-13T02:00:00.123Z","level":"INFO","service":"sfu","node_id":"sfu-a","pid":12,"tid":"1234","file":"NodeClient.cpp","line":80,"message":"Node registered"}
```

JSON 记录每条占一行，消息中的引号、反斜杠和换行等控制字符会转义；中文保持 UTF-8。
文本格式也带服务与节点身份，并转义消息中的控制字符。
源码位置由宏自动传入，UTC 时间以 `Z` 结尾。

集群进程启动完成后输出 `event=process_ready`、`message=PACKETIA_READY`，
并等待日志刷新。本地启动器识别 JSON、文本和旧版纯文本标记。
此事件承担启动握手职责，始终发出，不受 `PACKETIA_LOG_LEVEL` 过滤。
队列丢弃记录时产生 `event=logger_dropped`；输出失败时尝试向 stderr 报告 `logger_failed`。
第三方库及日志初始化失败时的兜底错误，仍可能输出普通文本。

`BaseLogger::WriteEvent()` 支持带类型的标量字段，放在 JSON 的 `data` 对象中。
整数、布尔、字符串和空值保留原类型；非有限浮点数输出 `null`。文本格式附加 `data={...}`。
日志元数据与业务字段分开，业务字段不会覆盖顶层 `event`、时间或进程身份。

媒体链路在 `PACKETIA_QOS_LOG=1` 时记录连接、房间/参会者映射、轨道状态、带宽分配、
探测以及每秒反馈/队列样本，仍受 INFO 级别过滤和同一有界日志队列约束。
详见[媒体质量诊断日志](../docs/media-quality-diagnostics.md)。其他请求的通用上下文跨线程传递仍待接入。

## 队列、轮转与验证

默认最多容纳 8192 条待处理记录、8 MiB 待处理字节，包含日志线程当前正在处理的批次。
达到上限时丢弃新记录；`stats()` 提供已接受、已写入、丢弃、输出失败和待处理量。
文件默认达到 16 MiB 时轮转，保留 3 个备份；文件缓冲每 200 ms 或显式 `flush()` 时刷新。
正常析构会排空队列；进程被强制终止时，尚未输出的日志可能丢失。

独立运行模块测试：

```sh
cmake -S log/Test -B build/logger-tests
cmake --build build/logger-tests
ctest --test-dir build/logger-tests --output-on-failure
```

测试验证 JSON 转义、运行时过滤、身份初始化、就绪事件、有界丢弃、并发输出和文件轮转。
根项目启用 `BUILD_TESTING` 时也注册 `StructuredLoggerTest`。
