# 一、目标定位

把现有 RTSP Server 改造成低延迟流媒体服务器  
特点：

- UDP / RTP 传输，减少 TCP 阻塞带来的延迟
- 最小缓冲，尽量减少端到端延迟
- 支持 H.264 / H.265 / Opus 等实时编码
- 能处理弱网丢包和简单 jitter buffer

---

# 二、阶段计划（2 周可完成原型）

## 阶段 1：分析现有 RTSP Server（1-2 天）

### 🎯 目标
- 理解现有架构
- 找出可能增加延迟的地方

### ✅ 任务
- 阅读 `RtspConnection::OnRead` / `read_buffer_` 的逻辑
- 确认 TCP buffer / 阻塞点
- 画一张 pipeline 图：

