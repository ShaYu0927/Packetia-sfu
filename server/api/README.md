# 账号 API 框架

账号业务与现有 SFU 在同一个进程中运行，`ServerApp` 负责装配。这里提供 HTTP 适配接口、账号路由和专用工作池，**尚未提供可用的 HTTP 监听器、密码验证或数据库实现**。默认关闭；当前显式启用不能启动一个假成功的登录服务。

## 分层和调用

```text
HttpServer（HTTP 协议与连接生命周期）
  → ApiServer（请求上限、专用工作池）
    → AccountController（路由、参数检查、业务错误映射）
      → AccountService（账号与登录会话）
```

`network/http/HttpServer.h` 定义异步请求/响应契约。计划使用 libwebsockets 实现解析和 writable 回调；当前 `LibwebsocketsHttpServer::Start()` 明确返回失败，不监听端口，也没有自行实现 HTTP 分帧。未来适配器应在读取阶段限制头部和 body、拒绝歧义头部，并设置连接和请求超时。

`AccountController` 已定义下列路由，但必须先实现监听器与业务依赖，才能通过网络使用：

| 方法和路径 | 语义 | 框架当前行为 |
| --- | --- | --- |
| `POST /api/v1/accounts` | 创建账号 | 校验参数后返回 501，等待注册/邀请/管理员策略 |
| `POST /api/v1/sessions` | 登录 | 调用 `AccountService::Login`，未配置返回 503，未实现返回 501 |
| `GET /api/v1/me` | 解析当前身份 | 需要登录会话凭据，再调用 `ResolveSession` |
| `POST /api/v1/sessions/renew` | 续期、轮换登录会话 | 需要登录会话凭据，再调用 `RenewSession` |
| `DELETE /api/v1/sessions/current` | 撤销当前登录会话 | 需要登录会话凭据，再调用 `Logout` |

登录 JSON 只允许 `login` 和 `password`；注册 JSON 额外允许可选 `display_name`。要求 `Content-Type: application/json`，拒绝未知字段、重复 JSON 键、类型错误和过大请求。其他三个接口不接收请求体。响应不缓存，认证失败统一返回 401，服务端错误不暴露数据库或凭据内容。

当前接口契约使用 `Authorization: Bearer <opaque-session-token>` 携带不透明的**业务登录凭据**，不处理媒体房间 JWT。没有实现 Cookie、CSRF、浏览器 token 持久化或会议 JWT 签发。浏览器产品接入前需要明确安全存储和同源策略；若改用 Cookie，应同时实现 Secure/HttpOnly/SameSite、CSRF 和 Origin 检查。

## 线程与生命周期

- HTTP I/O 回调仅移交有所有权的请求，账号工作池执行控制器与同步业务方法；媒体 I/O 线程不承担密码计算和数据库操作。
- 每个 `ApiServer` 注册独立、唯一名称的函数工作池，按递增 key 分配任务，使用 `DropTail` 并检查提交结果。
- `max_pending_requests` 限制该 API 实例所有工作线程**正在运行和排队的请求总数**；超额或投递失败立即返回 503。body 在入队前再次检查大小，生产适配器仍须在读入阶段实施限制。
- 响应回调可从工作线程调用；HTTP 适配器必须投递回原连接的 I/O 线程，检查请求代次和连接是否仍存活，再实际写回。关闭连接后丢弃晚到响应。
- `ServerApp` 先启动 `AccountService`，再启动 `ApiServer`。停止时 API 关闭新请求及监听器，排空工作池，之后才能关闭账号存储。监听启动失败会回滚已经启动的工作池。

## 后续实施顺序

1. 实现密码哈希、SQLite 表与迁移、账号业务事务、随机会话凭据及摘要存储。
2. 定义账号创建准入策略，再开放注册接口；加入登录限速和审计，日志不包含密码或完整凭据。
3. 完成 libwebsockets HTTP 适配器、异步回写、TLS/HTTPS 部署、连接超时及大小限制。
4. 接入会议成员管理与入会凭据签发，再让现有 WebRTC 鉴权消费会议授权。

当前测试使用内存假 HTTP 适配器验证边界与生命周期，不代表真实 HTTP、TLS、数据库或完整登录流程已经可用。
