问题背景：
在RtpThreadPool -> RtpVideoTracker::inputRtp() 处理链路中，偶发出现 len=0，
触发 inputRtp 内部 “len < 12 -> abort()” 的硬失败，导致进程直接崩溃。
经gdb回溯与参数检查，确认根因是 WorkJob.payload 语义不清，
存在将“原始字节buffer”错误 reinterpret_cast 成 Packet* 的情况，
从而导致 pkt->len 读取异常（常为0）。

核心原因：
- WorkJob.payload 原为 void*，缺乏类型语义约束
- 消费端存在 static_cast<Packet*>(job.payload) 的未定义行为
- payload_len 与真实数据来源存在双重真相，导致参数传递混乱

本次修改：
1. 将 WorkJob.payload 重构为：
      std::variant<std::monostate,
                   std::pair<uint8_t*, size_t>,
                   std::shared_ptr<Packet>>
   明确payload类型语义，消除非法强转路径。

2. 生产侧统一通过 std::shared_ptr<Packet> 传递内存池Packet对象，
   并使用自定义deleter自动归还内存池，移除裸指针释放逻辑。

3. 消费侧使用 std::get_if / std::visit 方式解析payload，
   统一以 Packet::len 作为真实长度来源，彻底避免 len=0 问题。

4. 移除 payload -> Packet* 的 static_cast，
   修复因参数非法导致的 inputRtp abort 崩溃问题。

5. logger 支持 std::variant 输出（泛化 shared_ptr 打印），
   避免基础模块依赖业务类型头文件。

效果：
- 消除 RTP 处理链路中因参数错误导致的 SIGABRT 崩溃
- 明确跨线程payload生命周期与类型语义
- 提升RTP接收处理链路的健壮性与可维护性

---
背景：
推进RTP接收链路，实现从裸RTP包解析 → jitter缓存 → 顺序输出 → H264 NALU组帧，
为后续解码、转发和录制打通稳定基础链路。

feat(rtp): 实现RTP解析+jitter排序+H264 NALU组帧

- 完成 RTP 头解析与参数校验
- 接入 EnhancedPacketSortor 实现 jitter buffer
- 新增 onRtpSorted，按序输出 RTP
- 支持 H264 单包 / STAP-A / FU-A 组帧
- 输出 AnnexB 格式帧数据

---
debug: 验证 RTP pipeline 全链路，定位 PacketPool exhausted 根因方向

- 使用 gdb 逐级确认 RTP 包从 worker 投递至 RtpVideoTracker::onRtpSorted
- 打通 inputRtp -> sortor -> emit -> callback -> video tracker 处理链路
- 明确回调正常触发，排除链路中断问题
- 为后续修复排序缓存与 PacketPool 回收问题提供依据
