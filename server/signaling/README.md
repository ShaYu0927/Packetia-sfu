# 房间信令与轨道事件

## 当前请求链路

客户端先通过 `join` 入房，再用 `publish` 携带 Offer 发布，或用 `subscribe` 携带 Offer
和 `tracks: [{track_id, mid}]` 指定业务轨道到订阅端 MID 的绑定。服务端返回 Answer。
`tracks` 请求返回当前房间的轨道列表。现有单连接只支持一个发布或订阅媒体会话，
动态添加轨道的重协商尚未开放。

业务调用链：

```text
WsSignalGateway 解码
  → SignalSession 请求通道
  → RoomSignalHandler（owner）
  → WebRtcSession 协商 / Room::PublishTrack
  → Room 登记轨道并发布 TrackPublishedEvent
  → Bus 回调更新房间轨道版本
  → 返回 Answer，经响应通道发送
  → 后续 Tick 生成 tracks_changed，经同一响应通道发送
  → Gateway 在连接执行器串行写 WebSocket
```

## 主动通知

轨道列表发生变化后，同房间已加入且在线的参与者会收到：

```json
{
  "type": "tracks_changed",
  "room_id": "demo",
  "revision": 2,
  "tracks": [
    {"track_id": "p-example:a", "publisher_id": "p-example", "kind": "audio", "mime_type": "audio/opus"}
  ]
}
```

`tracks` 是当前完整列表，可以为空；客户端应替换本地列表。
同一 Tick 前多次变化会合并，不能把该通知作为逐条增删日志。
`revision` 只在本次房间实例内有效，重新入房时应重新获取列表。
通知仍检查 ListTracks 权限，只发送给当前房间；客户端必须先识别通知类型，
不能把它交给正在等待 Answer 等请求响应的 Promise。示例 `app.js` 已处理这种分流。

初次加入仍用 `tracks` 请求获取列表。恢复信令时先返回 `resumed`，
如果房间发生过轨道变更，随后推送当前快照；恢复期间不向旧连接发送通知。
信令短暂断开保持原媒体和轨道；恢复超时、显式离房、媒体终止触发原有清理及移除事件。

## 线程、资源与失败处理

Room 的 Bus 回调在房间锁外同步执行。信令订阅者只通过弱引用更新原子版本槽，
不捕获 handler、房间或连接，不发送网络消息。每个房间仅保存一个版本槽，
每个参与者仅保存已投递版本；所有房间和参与者状态由现有 owner 执行器处理。

`RoomSignalHandler::Tick` 将最新列表投递到现有响应通道，沿用初始响应门控、
连接代次校验、消息数/字节数限制和失败取消机制。入队失败不推进已投递版本；
仍可用的通道下次检查最新状态，已取消的通道由既有关闭流程回收。
发布请求回滚时，通知依据 Tick 时的最终列表，不会把中间发布记录当作可用轨道发送。

离房时回收空房间订阅，停服先解除 Bus 订阅，再清理房间和端点。
Room 仍负责清理路由；Bus 传播已提交的状态变化，不承担必需清理步骤的调度顺序。

## 范围

本次接通房间发布/移除事件与客户端列表通知。没有增加订阅自动重试、
动态权限更新、静音转发控制、轨道级关闭状态机或 RTSP 流状态事件。
收到通知更新列表也不等于已经协商订阅；建立媒体路由仍须现有 subscribe 流程。
