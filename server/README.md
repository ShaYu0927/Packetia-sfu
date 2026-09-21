# 服务启动

按下面的顺序阅读即可：

| 文件 | 职责 |
| --- | --- |
| `main.cpp` | 加载配置，启动、运行、停止应用 |
| `ServerConfig.h/.cpp` | 默认监听地址、端口、I/O 线程数和录制配置；读取环境变量 |
| `ServiceNames.h` | 服务注册名称的统一宏定义，例如 `SERVICE_RTSP` |
| `ServerApp.cpp` | 创建共享资源，组装媒体服务和网络服务，确定依赖顺序 |
| `WorkerSetup.cpp` | 配置常驻线程池和任务处理器 |
| `ServerLauncher.h/.cpp` | 通用服务注册、顺序启动、失败回滚、逆序停止 |

启动顺序为：

```text
EventLoop → 帧发布器 → 录制 / AI → 常驻线程池 → 网络服务
```

停止时按相反顺序执行。网络关闭可能投递媒体清理任务，因此先停止网络，
再排空工作池；这期间帧消费者和 I/O 仍然可用。最后解除帧发布器绑定并停止 I/O。

新增服务时，在 `ServerApp` 对应的组装函数中注册：

- 实现 `IService` 的业务服务使用 `AddService()`，统一执行 `Init/Start` 和 `Stop/Shutdown`。
- 通过地址、端口启动的网络服务使用 `AddIpPortService<T>()`。
- EventLoop 等接口不同的资源使用 `AddCustomService()`。

服务名必须唯一。服务启动返回失败或抛异常时，也会调用它的清理函数，
再逆序清理此前启动的服务。清理函数需要支持部分初始化的状态；单个清理异常会记录日志，
其余服务继续清理。注册和启停操作由同一个控制方串行执行。

现有环境变量保持有效：`PACKETIA_RECORDING=0` 关闭录制，
`PACKETIA_RECORD_DIR` 设置录制目录。WebSocket 仍由构建选项控制。

启动器测试可独立运行：

```sh
cmake -S server/Test -B build/server-tests
cmake --build build/server-tests
ctest --test-dir build/server-tests --output-on-failure
```
