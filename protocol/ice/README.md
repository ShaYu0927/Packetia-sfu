# SFU ICE 接入

当前 `WebRtcSession` 在 SDP answer 中声明 `a=ice-lite`，`IceAgent` 是对应的
被动 STUN Binding 请求处理器。适用于客户端能到达服务端候选地址的部署；
浏览器承担完整 ICE 的连通性检查与提名职责。本模块不是完整 P2P ICE 引擎。

## 处理流程

1. 校验 STUN 报文边界及 FINGERPRINT，损坏的数据报直接丢弃。
2. 校验 `USERNAME = 本地 ufrag:远端 ufrag`，使用本地密码校验 MESSAGE-INTEGRITY。
3. 校验 PRIORITY、角色与 USE-CANDIDATE；未知必理解属性返回 420。
4. 返回包含 XOR-MAPPED-ADDRESS、MESSAGE-INTEGRITY 和 FINGERPRINT 的成功响应。
5. 只有认证通过的 controlling 对端携带 USE-CANDIDATE 时，才选择其源地址。
   普通连通性检查不切换地址；同一地址的重复提名继续响应，但不重复通知。
6. WebRtcSession 发送响应后，将选中地址交给 WebRtcTransport，再启动 DTLS。

HMAC 输入不包含 MESSAGE-INTEGRITY 属性本身，但头部长度包含该属性。
MESSAGE-INTEGRITY 后的普通属性不参与 ICE 决策，防止未认证的尾部属性触发提名。
认证失败不会学习远端凭据；变更本地或远端凭据会清除 Agent 内的地址选择。
400/401 响应不附加 MESSAGE-INTEGRITY；认证后的 420/487 响应带完整性校验。

所有 Agent 操作及回调必须在会话事件循环串行执行。SFU 使用 Controlled 角色，
对端也声明 Controlled 时返回 487；保留的角色和 tie-breaker 配置接口不代表
已经实现完整 ICE 的角色切换、检查调度与事务重试。

## 借鉴与验证

- [Pion ICE](https://github.com/pion/ice/blob/master/agent.go)：参考认证后再处理
  对端状态、候选与角色的处理顺序，不直接移植其完整 P2P 检查器。
- [RFC 8489 §14.5](https://www.rfc-editor.org/rfc/rfc8489.html#section-14.5)：
  MESSAGE-INTEGRITY 计算规则。
- [RFC 5769 §2.1](https://www.rfc-editor.org/rfc/rfc5769.html#section-2.1)：
  使用独立固定报文验证 HMAC 和 FINGERPRINT，避免只用自身构造器自测。
- [RFC 8445](https://www.rfc-editor.org/rfc/rfc8445.html)：ICE 检查、角色与提名规则。

```sh
cmake --build build/macos --target ice_unit_tests test_webrtc_session
ctest --test-dir build/macos --output-on-failure \
  -R '^(IceChecklistUnitTest|test_webrtc_session)$'
```

测试覆盖标准报文、错误密码、凭据污染、未认证尾部提名、重复提名、地址变更、
角色冲突、属性错误、凭据更新和会话内的 STUN 到 DTLS 启动路径。
WebRTC 会话测试使用测试加密后端，不等同于真实浏览器互通验证。

## 尚未完成

- 应用层周期调度 `WebRtcSession::Tick`（当前会话仍由应用组装）。
- 会话级 ICE restart：Agent 清除选择不等于 WebRtcTransport 与信令已完成重启。
- 完整 ICE 的主动检查、事务重传、候选收集、TURN 分配及 ICE-TCP。
- 浏览器与实际 DTLS/SRTP 后端的端到端互通。

现有 IceChecklist/Candidate 辅助类尚未接成完整主动检查状态机。

## ICE-Lite 存活检测

### 公共状态机接入

`IceAgent` 使用 `utils::StateController` 驱动生命周期，`CurrentState()` 可查询状态：

| 状态/事件 | 结果 |
|---|---|
| New + StartLiveness | Checking，启动建立期限 |
| New + 有效普通检查 | Checking（兼容未启动计时的独立 Agent 用法） |
| Checking + 有效提名 | Completed，记录选中地址 |
| Completed + 有效检查或提名 | 保持 Completed，按选中地址刷新计时 |
| Completed + 凭据变化 | Checking，清除选中地址；不自动重置期限 |
| Checking / Completed + 超时 | Failed，清除选中地址 |
| 非 Closed + 显式 StartLiveness | Checking，重新启动计时并清除选择 |
| 任意状态 + Close | Closed，停止计时并清除选择 |

Failed 不会因检查包或凭据更新复活，但独立 Agent 允许显式 StartLiveness。
Closed 是终态，不能重新启动。`StopLiveness()` 作为 `Close()` 的兼容别名，
不再表示暂停计时。WebRtcSession 的 Shutdown 显式调用 Close；失败的会话仍需新建。
`HasSelectedPeer()` 从 Completed 状态派生，不再单独维护可与生命周期冲突的布尔标志。
选中地址回调触发前，状态和地址均已更新。查询与转换都必须在拥有者线程执行。

`WebRtcSession::start()` 启动存活计时，默认 `iceTimeoutMs = 30000`，必须大于零。
初次提名也受这个期限限制，未提名的普通检查不会无限延长连接建立时间。
提名后，只有选中地址发来的、认证及属性校验通过的 Binding 请求刷新计时；
普通检查和重复提名都可以刷新。错误密码、其他地址的普通检查、RTP/RTCP
与 DTLS 流量都不能刷新。合法的新地址提名会切换地址并刷新计时。

应用应在会话事件循环上周期调用 `Tick(nowMs)`，例如每秒一次，确保完全无流量时
也能发现超时。ICE 自身使用 `steady_clock`，可通过 `iceClock` 注入单调时钟做测试；
`Tick` 的参数继续传给 DTLS 后端，不用于改写 ICE 时钟。超时边界为 `elapsed >= timeout`。
即使 Tick 延迟，入站数据和 `SendRtp/SendRtcp` 也会在处理前检查期限。

超时使会话进入 `Failed`，错误为 `ICE connectivity check timeout`，关闭 transport、
DTLS 和 SRTP 并解除收包绑定。后续数据不会复活会话，需要建立新会话。
这只是 ICE-Lite 的入站检查存活策略，不是完整 ICE 的主动 consent freshness
请求/响应事务实现，也不提供超时后的自动恢复或同会话 ICE restart。

单元测试使用假时钟，覆盖首次提名超时、有效检查续期、错误密码与异地址不续期、
地址切换和过期不可复活；会话测试覆盖 Tick、收包、RTP/RTCP 发送触发的超时清理。
