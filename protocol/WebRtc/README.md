# WebRtcSession 接入

`WebRtcSession` 实现 ICE-lite answerer 的会话流程。DTLS 和 SRTP 是抽象接口，
由应用注入真实后端；项目没有默认的空加密实现。

## 各函数职责

| 函数 | 行为 |
| --- | --- |
| `ApplyRemoteOffer` | 校验并保存 Offer，合并会话级 ICE/DTLS 默认值，检查 MID、BUNDLE、指纹、角色和 RTP 参数。失败不覆盖已有 Offer。 |
| `CreateLocalAnswer` | 与本地能力求交集，确定收发方向和 DTLS 角色，使用本地 ICE 凭据及证书指纹生成 Answer；保留 m-line 顺序，不支持的媒体使用端口 0 拒绝。 |
| `start` | 配置 ICE controlled 角色和 DTLS 后端，绑定 transport sink，开始等待 ICE。此时尚未 Connected。 |
| `HandleStun` | 调用 IceAgent 校验请求并向请求源发送响应；成功提名对端后选择 peer，启动 DTLS。 |
| `HandleDtls` | 把选定对端的数据交给后端；证书指纹验证成功后导出 SRTP 密钥，安装收发上下文，再进入 Connected。 |
| `HandleEncryptedRtp/Rtcp` | SRTP 就绪后认证、解密；失败直接丢包。RTP 还检查协商的 payload type 和接收方向，成功后转交媒体 sink。 |
| `SendRtp/SendRtcp` | 检查连接状态，调用 SRTP 加密和认证，再发送到选定 peer。RTP 受协商方向限制，RTCP 不受媒体方向限制。 |
| `Tick` | 驱动 DTLS 重传与超时，处理由定时器完成的握手。 |
| `stop` | 解绑 sink、关闭会话传输、释放加密上下文；可重复调用。关闭后需创建新会话。 |

状态流程：`New -> HaveOffer -> HaveAnswer -> Connecting -> Connected`。
运行中致命错误进入 `Failed` 并清理；主动停止进入 `Closed`。失败原因通过
`LastError()` 获取。SDP 校验失败只返回 false，不关闭尚未启动的会话。

## 创建和调用

应用提供 `WebRtcSessionOptions`：

- `ice`：每个会话新生成的随机 ufrag/password、收集完成的本地 candidates。
  candidate 保存不带 `a=candidate:` 的属性值；当前没有 trickle 接口。
- `medias`：每种媒体的本地能力，包括 `sdp.media`、收发方向、codecs、RTCP
  feedback、headerExtensions，以及需要发送时的本地 SSRC/MSID。
- `origin`：可选的本地 SDP origin；未填写的字段会生成默认值。

输入 Offer 必须已经填好 `medias` 中的结构化字段，包括 `sdp` 的媒体类型、
端口、协议、格式列表，以及 codecs、mid、direction、rtcpMux 等。
`sdp::Sdp::Parse()` 当前只提供基础 SDP 解析，不自动提取这些 WebRTC 字段。

```cpp
// 应用先注册并启动 endpoint 和 media worker。
auto ingress = std::make_shared<media::transport::MediaEndpointIngress>(endpoint->Id());
auto session = std::make_shared<protocol::webrtc::WebRtcSession>(
    transport, std::move(dtlsBackend), std::move(srtpBackend),
    ingress, std::move(options));

protocol::webrtc::WebRtcSessionDescription answer;
if (!session->ApplyRemoteOffer(offer) ||
    !session->CreateLocalAnswer(answer) || !session->start())
{
    // 向调用者报告 session->LastError()。
    return;
}
// 通过信令发送 answer；定期调用 session->Tick(monotonicNowMs)。
```

所有会话操作、收包回调和 Tick 必须在同一事件循环串行执行。
SFU 媒体线程发送反馈时，需要把 `SendRtcp` 调用调度回该事件循环。
会话负责自己的 WebRtcTransport，底层共享 UDP socket 的生命周期仍由应用管理。
`MediaEndpointIngress` 保留数据所有权并按现有媒体线程亲和规则投递；会话不负责
全局 endpoint 注册、启动或停止。

## 后端约定和当前边界

- `DtlsTransport::LocalParameters()` 返回实际握手证书的指纹，证书身份在协商和
  握手之间保持不变。只有握手及远端指纹验证都成功时 `IsConnected()` 才能返回 true。
- `ExportSrtpKeys()` 根据 DTLS 客户端/服务端角色拆分 exporter 输出，填写带 salt 的
  本地发送和接收密钥以及 protection profile。SRTP 后端校验 profile 和密钥长度。
- `Unprotect*` 必须完成认证和重放保护，不得把认证失败的数据当作明文交给会话。
- 后端 `Close()` 必须幂等；DTLS 的发送回调在会话事件循环执行。
- 当前支持 UDP/TLS/RTP/SAVPF、rtcp-mux、一个传输及可选的单个 BUNDLE 分组。
  BUNDLE 内需要相同的 ICE/DTLS 参数和不冲突的 payload type。
- codec 名称忽略大小写，clock rate/channels/fmtp 按兼容值严格匹配。
  未实现 codec 专用 fmtp 协商、RTX/RED/FEC、SCTP、MID 解复用、重新协商、
  ICE restart、trickle signaling 或主动 ICE 检查。完整的 consent freshness
  和 ICE 等待超时策略仍需后续接入。
- Answer 同时填充结构化字段和 `answer.sdp`。项目现有 `sdp::Sdp::Serialize()`
  仍返回空字符串，发送 SDP 文本前需实现序列化；本次没有修改通用 SDP 模块。

参考：[Offer/Answer](https://www.rfc-editor.org/rfc/rfc3264)、
[DTLS-SRTP](https://www.rfc-editor.org/rfc/rfc5764)、
[BUNDLE](https://www.rfc-editor.org/rfc/rfc9143)。

## 验证

`test_webrtc_session` 使用 DTLS/SRTP 测试后端验证协商、收发门控、错误清理和生命周期。
测试中的恒等解密不用于产品代码。具备 OpenSSL headers 时另执行真实 STUN
MESSAGE-INTEGRITY 与 USE-CANDIDATE 提名测试；没有 OpenSSL 时会明确打印跳过信息。
这些测试不代替真实 DTLS/SRTP 后端和浏览器互通测试。
