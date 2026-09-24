# RTSP 推流会话状态机

`RtspSession` 使用公共 `utils::StateController`；转换表位于 `RtspSession.h`。
状态只描述控制会话阶段，RTP/RTCP 通道、每条轨道和 transport 仍由现有媒体对象管理。
所有状态访问与请求处理必须在连接所属线程串行执行。

| 当前状态 | 成功事件 | 下一状态 |
|---|---|---|
| Initial | ANNOUNCE | Announced |
| Announced | 再次 ANNOUNCE | Announced |
| Initial / Announced | SETUP | Ready |
| Ready | 后续轨道 SETUP | Ready |
| Ready / Recording | RECORD | Recording |
| Ready / Recording | TEARDOWN | Initial |
| 任意状态 | TCP 断开 / 析构 | Closed |

Initial 允许直接 SETUP 已注册的媒体描述，保留原有接入方式。
后续 SETUP 允许省略 Session 以兼容现有客户端；如果提供，必须匹配当前会话。
RECORD、TEARDOWN 必须携带本连接持有的 Session ID，不能操作其他连接的媒体会话。
请求先检查允许的阶段，业务处理成功后才提交成功事件；失败 SETUP 保留原有轨道和状态。

Recording 阶段拒绝 ANNOUNCE 与新增 SETUP（455）；重复 RECORD 返回成功。
TEARDOWN 清理 transport、endpoint、通道和请求对象缓存，释放控制会话状态后允许
同一 TCP 连接重新 ANNOUNCE/SETUP。重复 TEARDOWN 因已无会话而返回 454。
TCP 断开进入 Closed，重复清理幂等；关闭后不再派发新请求。

本次没有实现播放或暂停：DESCRIBE、PLAY、PAUSE 返回 501。
状态机也不新增 RTP 入站门控，保持已有 SETUP 后可接收媒体的行为，
Recording 表示 RECORD 控制请求已成功，不是首个媒体包到达的标志。

验证：`test_udp_binding` 包括真实 TCP 控制连接的错误顺序、畸形 Session ID、
失败回滚、多轨共享端点、重复 RECORD、TEARDOWN 后重建，以及原有 UDP 媒体收发测试。
