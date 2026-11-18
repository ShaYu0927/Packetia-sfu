# 从 WebRTC 源码学习趋势检测

这一轮直接引入 WebRTC 的 `TrendlineEstimator`，固定上游提交
`afef16afa42fb248d85c717bfbd88d612a37722a`。原始文件保留在
[upstream](../third/webrtc_trendline/upstream/modules/congestion_controller/goog_cc)，
生产使用的适配版在 [port](../third/webrtc_trendline/port/trendline_estimator.cc)。
版本、原文件哈希、许可证和本地改动说明见[第三方模块 README](../third/webrtc_trendline/README.md)。

## 先按调用链读三个文件

```text
TransportBweController::OnTransportFeedback()
    → PacketGroupObserver::OnFeedback()
        → 排序、去重、完成包组
        → ObserveCompletedGroup() 计算相邻组的 ΔR、ΔS
            → WebRtcTrendlineEstimator::Update()
                → 引入的 TrendlineEstimator::Update()
                    → UpdateTrendline()
                    → Detect()
                    → UpdateThreshold()
        ← PacketGroupObservation.usage
    → 现有 RemoteBitrateController::Update()
    → BweResult → NetworkController → 订阅预算和 Pacer
```

1. [PacketGroupObserver.cpp](../media/quality/bwe/PacketGroupObserver.cpp)：先找
   `ObserveCompletedGroup()`。这里保留本项目的分组、组间差计算、乱序检查和长间隔重置，
   原来内嵌的累计、EWMA、回归、过载检测已交给移植模块，避免重复平滑。
2. [WebRtcTrendlineEstimator.cpp](../media/quality/bwe/WebRtcTrendlineEstimator.cpp)：这是项目适配层。
   它把接收时钟归一到相对原点、映射枚举，并在满 20 个组间样本前返回空值。
   绝对时间可以大于 `INT64_MAX`；相减后再转换，避免直接转换时溢出。
3. [trendline_estimator.cc](../third/webrtc_trendline/port/trendline_estimator.cc)：这是实际算法。
   建议同时打开对应的上游文件，用 `diff` 查看哪些外围依赖被移除。

适配层给上游接口的 `send_time_ms` 和 `packet_size` 传 0：上游在没有预测器的默认路径中
不使用这两个参数。有效输入是组间发送间隔、组间接收间隔及接收时间轴，单位均为毫秒。
这些时间不能替换成 RTCP 到达本机的时间，也不能先做一轮平滑再输入。

## 再读算法的四个位置

`UpdateTrendline()` 先计算：

```cpp
delta_ms = recv_delta_ms - send_delta_ms;
accumulated_delay_ += delta_ms;
smoothed_delay_ = 0.9 * smoothed_delay_ + 0.1 * accumulated_delay_;
```

例如发送组间隔为 20 ms、接收组间隔为 25 ms，本次增加的相对延迟为 5 ms。
累计值表示延迟相对起点的变化，不能当成绝对单向时延。

接着看 `LinearFitSlope()`：它以相对接收时间为 x、平滑累计延迟为 y，在 20 个样本上
做最小二乘回归。正斜率表示延迟增加，负斜率表示下降；斜率本身的单位是 ms/ms。

再看 `Detect()`：斜率经过样本数和经验系数缩放后与阈值比较，还要检查持续时间、
连续次数及趋势变化，才能判断 Overusing。触发后计时器和计数器也会按上游逻辑更新。
这里的检测量不能按“多少毫秒延迟”展示。

最后看 `UpdateThreshold()`：阈值跟随一般的检测量变化，遇到大尖峰时跳过适应，
并保留时间步长及阈值上下限。这些内部状态和更新顺序都是算法的一部分。

## 接入后的边界

- 默认配置固定为上游默认参数，无实验字符串配置、无可选网络预测器。
- 项目继续使用自己的 5 ms 包分组，尚未引入 WebRTC 的 `InterArrivalDelta`。
- 包组观察间隔超过 2000 ms 或路径重置时，重新构建趋势检测状态。
- 项目保持“20 个有效组间样本后才输出分组判断”的约定；此前及分组没有新判断时，
  `TransportBweController` 仍保留原来的逐包检测回退。
- 接收码率估计、`RemoteBitrateController`、探测和发送队列仍使用现有实现。
  因此这次引入的是一个 GCC 子算法，不是完整 libwebrtc GCC。
- 原有手写检测的过载计数、计时和样本计数处理与上游有差异；接入后以固定版本的
  上游状态机为准，不承诺与旧算法的所有判定时刻相同。

## 如何验证、如何单步学习

```sh
cmake -S . -B build/webrtc-integration
cmake --build build/webrtc-integration --target test_webrtc_trendline \
  test_weak_network_manual test_rtp_input PacketiaSfu --parallel
PACKETIA_LOG_MODE=console ctest --test-dir build/webrtc-integration --output-on-failure \
  -R '^(webrtc_trendline|test_weak_network_manual|test_rtp_input)$'
```

第三方模块也能独立以 Debug 构建，步骤见其 README。在 Debug 测试中，依次给
`UpdateTrendline()`、`LinearFitSlope()`、`Detect()` 和 `UpdateThreshold()` 下断点。
上游的 `Normal`、`Overusing`、`Underusing` 测试分别提供稳定、增长和下降的输入。

测试同时编译未改动的上游算法和适配算法，在空实验配置、无预测器条件下，逐步比较
2400 次输入的状态、平滑累计延迟、斜率和阈值。参考算法的外围依赖由仅用于测试的
默认配置兼容头提供；这项对照不覆盖其他 WebRTC 实验配置或整个 WebRTC 运行时。
项目测试还覆盖大接收时钟、重置、时钟倒退、分组预热、乱序、丢包和长时间中断。
