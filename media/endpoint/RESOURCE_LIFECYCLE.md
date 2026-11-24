# 房间与媒体资源生命周期

业务授权、信令连接和媒体连接分别管理寿命。账号登录会话提供业务身份，入会令牌授权加入指定房间；
WebSocket 连接承载信令，发布和订阅各使用独立的 WebRTC 会话。

## 谁拥有资源

| 对象 | 拥有或持有的资源 | 引用与清理边界 |
| --- | --- | --- |
| `AccountService` / `AccountStore` | 登录会话记录、令牌摘要与有效期 | 不直接拥有 SFU 成员、WebSocket 或媒体连接 |
| `RoomSignalHandler` | 本 owner 的成员映射、房间及发布/订阅 `WebRtcSessionContext` | 在固定 room owner 上处理恢复、媒体停止与完整退出 |
| `Membership` / `Room` | 成员身份、信令绑定代次、恢复期限、发布/订阅关系 | 连接断开不等于成员离开；Room 不负责关闭 WebRTC 协议栈 |
| `WebRtcSessionContext` | WebRTC 会话、传输适配器、Ingress、Endpoint，发布方向还持有 `MediaStream` | 服务层统一执行关闭；不依赖 `shared_ptr` 析构触发业务退出 |
| `ParticipantSession` | 固定角色、会话 ID、Endpoint 强引用 | 房间可访问媒体端点；不持有 ICE、DTLS、SRTP 或协议上下文 |
| `WebRtcSession` | ICE 状态、DTLS、SRTP、会话传输 | `stop()` 断开传输入口并关闭协议资源；共享 UDP 监听由服务管理 |
| `WebRtcMediaTransport` | 发送侧控制器和有界 Pacer | 弱引用协议会话与接收入口；`Close()` 关闭入口、清空队列，并请求 owner 停止协议会话 |
| `DownlinkTrackManager` | 活动订阅、RTCP 发送索引、带宽分组与观察者、下行协商参数 | 参数仅弱引用连接；活动订阅的发送器才持有传输引用 |
| `Subscription` | 本订阅的发送器、消费闸门和发送包装器 | 弱引用发布端和订阅端；发布轨道也弱引用订阅，避免引用环 |

`EndpointManager` 和 `MediaStreamRegistry` 也可能持有对象，因此移除服务映射并不代表对象立即析构。
必须先显式关闭，再撤销注册。外部引用或已排队任务可以暂时保留对象内存，但不能使关闭的连接或轨道恢复发送。

## 各种操作释放什么

| 操作 | 保留 | 关闭或移除 |
| --- | --- | --- |
| WebSocket 临时断开 | 成员身份、恢复令牌、现存发布/订阅会话与轨道 | 当前信令绑定；未完成的协商回滚，启动恢复期限 |
| 在期限内恢复 | 原成员及仍有效的媒体会话 ID | 旧信令连接绑定失效；新绑定递增代次，客户端按恢复结果进行 ICE restart |
| 取消一条端点订阅 `SfuEndpoint::Unsubscribe` | 共用传输、仍有效的协商参数、其他订阅 | 该路由闸门、RTCP 发送索引、对应带宽需求；最后一条订阅移除传输组观察者 |
| WSS `unsubscribe` | 成员与发布方向 | 整个订阅 WebRTC 会话、其中的路由与端点；重建需重新协商 |
| WSS `unpublish` | 成员与订阅方向 | 发布会话、发布流和轨道；对应下游消费关系关闭 |
| `leave`、恢复超时、移除成员 | 其他成员及其独立资源 | 该成员两个方向的媒体、轨道关系和身份；最后一名成员退出会退休房间分配 |
| `end_room` | 其他房间 | 先退休该房间分配，再清理全部成员；旧分配令牌不能重建房间 |
| 服务停止 | 无运行中的本服务会话 | 取消服务通道，清理全部成员和媒体，关闭所拥有的共享监听 |

下行协商配置不是连接所有者。创建配置的调用方必须持有传输对象。
取消路由后，只要连接仍由会话拥有，就能按原参数重新订阅；连接引用耗尽后，旧参数不能复活传输，
下一次 `Prune()` 会移除过期参数，释放其 SSRC 占位。

## 完整退出的顺序

`RoomSignalHandler::RemoveSession` 在固定 owner 上执行：

1. 从会话映射摘下该方向，防止后续请求命中它。
2. `WebRtcMediaTransport::Close()` 先关闭发送闸门、接收入口及 Pacer 队列。
3. 在 owner 上同步 `WebRtcSession::stop()`，关闭会话传输、ICE、DTLS 与 SRTP；不能只依赖投递的异步 stop。
4. 撤销本会话发布/订阅的房间轨道关系，按精确绑定解除对应媒体角色。
5. 注销 UDP mux 的 ufrag 和 Endpoint 注册，停止 Endpoint。
6. 按发布流实例撤销注册，避免旧实例回调移除同名的新流。
7. 两个媒体方向清理后，再将成员移出房间，释放参与者容量。

取消单条订阅只关闭本订阅闸门，不调用底层共用传输的 `Close()`。
重复关闭是幂等的；旧信令关闭使用绑定代次校验，旧媒体停止请求使用会话 ID 校验。
排队媒体在执行时检查路由闸门，WebRTC 排队发送还检查连接是否关闭及路径 epoch，防止旧任务跨恢复或切网发送。
已经交给 socket 的数据无法撤回，这里的取消覆盖尚未执行的排队任务。

## 当前边界

- 账号注销/登录会话过期不会自动踢掉已加入 SFU 的成员。HTTP 登录会话、入会 JWT 与房间运行时分别管理；
  如需“注销即踢人”，应由业务层发出移除成员命令，并另行处理已签发令牌的撤销策略。
- 恢复限于同进程、同房间 owner，服务重启和跨节点迁移不保留媒体状态。
- 单独调用 `Room::Close()` 只清理核心房间和端点。外部结束 WebRTC 房间必须走服务的 `end_room`，
  才会执行协议、mux、全局注册与容量的完整清理。
- 关闭闸门与实际释放内存是两个时刻。已在执行中的工作可能完成，关闭后的排队工作应被丢弃。

## 回归入口

```sh
cmake --build build/webrtc-integration --target test_rtp_input test_webrtc_service --parallel 2
ctest --test-dir build/webrtc-integration --output-on-failure \
  -R '^(test_rtp_input|webrtc_service|webrtc_members|webrtc_control|webrtc_duplex)$'
```

`test_rtp_input` 覆盖参数弱引用、过期参数清理、重复订阅隔离、排队重传/音频取消、旧信令绑定与角色独立性。
`webrtc_control` 在有活动发布及断线订阅的情况下结束房间，并保留外部端点引用验证其已停止、轨道已清理、注册已移除。
`webrtc_duplex` 覆盖双方向恢复、权限撤回、恢复超时与服务停止。
