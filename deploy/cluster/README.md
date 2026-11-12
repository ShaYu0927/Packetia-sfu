# Packetia 集群启动与 Docker 部署

第一版部署形态为单台 Linux 服务器上的一个 `PacketiaControl` 和两个 `PacketiaSfu` 容器。
房间由控制服务分配到节点，同一房间固定到同一节点。公网入口使用 Caddy 自动申请和续期证书。

```text
客户端 ── HTTPS / WSS :443 ── Caddy
                              ├─ /api/*          → control:18082
                              ├─ /sfu-a/webrtc   → sfu-a:18180/webrtc
                              └─ /sfu-b/webrtc   → sfu-b:18181/webrtc
客户端 ── SRTP / UDP :19100 ─────────────────────→ sfu-a
客户端 ── SRTP / UDP :19101 ─────────────────────→ sfu-b

SFU 注册、续租、房间管理 → Docker 内部网络
账号与房间分配         → control-data 持久卷
证书                   → caddy-data 持久卷
```

## 文件职责

| 文件 | 职责 |
| --- | --- |
| `deploy.py` | 生成配置和密钥，统一构建、启动、检查、日志与停机命令 |
| `Dockerfile` | 多阶段构建 Control/SFU 共用镜像，运行时使用非 root 用户 |
| `compose.yaml` | Control 和两个 SFU，定义持久卷、就绪检查与启动依赖 |
| `compose.public.yaml`、`Caddyfile` | 公网 HTTPS/WSS 路由及证书持久化 |
| `control.json`、`sfu-a.json`、`sfu-b.json` | 配置生成模板；实际运行配置位于 `runtime/` |
| `.env.example` | 环境变量说明；部署时通过 `init` 生成随机密钥 |

若只需要用原生进程调试，可在仓库根目录运行：

```sh
python3 scripts/run_cluster_local.py --build-dir build/dev --nodes 2
```

该命令需要先构建 `PacketiaControl` 和 `PacketiaSfu`，不依赖 Docker。

## 服务器准备

- 安装 Docker Engine、Docker Compose V2 插件（2.20 或更高）及 Python 3.9 或更高。
- 将代码放到服务器；镜像默认在服务器构建，也可先构建后分发同一个 Control/SFU 镜像。
- 将域名的 DNS A 记录指向服务器公网 IPv4；`public_ip` 使用客户端可达的服务器地址。
- 在服务器防火墙和云安全组放行 TCP `80`、`443`，UDP `19100`、`19101`。
  80 用于证书申请与 HTTPS 跳转，443 用于 API 和 WebSocket，UDP 用于实际媒体。

内部注册端口、节点管理端口和指标端口不映射到公网。HTTPS 反向代理不转发媒体 UDP。
本模板使用 UDP 直连；客户端网络禁止 UDP 时，需要额外部署并接入 TURN。

## 公网启动

从仓库根目录执行，替换示例域名和文档地址：

```sh
python3 deploy/cluster/deploy.py init \
  --domain rtc.example.com \
  --public-ip 203.0.113.10

python3 deploy/cluster/deploy.py up --build
python3 deploy/cluster/deploy.py check
curl --fail https://rtc.example.com/healthz
```

`init` 只生成配置；`up` 才启动服务。首次构建耗时取决于服务器 CPU 和网络。
构建默认并发 2，可在生成的 `.env` 中调整 `PACKETIA_BUILD_JOBS`。
`up` 先等待控制服务健康，再启动并等待两个 SFU 完成注册，最后启动 HTTPS/WSS 入口。

生成目录为 `deploy/cluster/runtime/`：

| 文件 | 内容 |
| --- | --- |
| `.env` | JWT 密钥、两份独立节点密钥、域名、镜像与项目名称；权限 0600 |
| `control.json` | 控制服务监听、账号和集群数据库、注册开关 |
| `sfu-a.json`、`sfu-b.json` | 节点身份、公网媒体地址、对外信令 URL、内部管理与指标地址 |

重复执行 `init` 会保留密钥，重新生成 JSON 配置；数据库保存在独立持久卷中。
此目录已从 Git 和 Docker 构建上下文排除。更换服务器或恢复部署时，要一并保留 `.env` 和数据卷。
修改 JSON 配置后，使用 `up --recreate` 重新加载。

本框架提供集群后端 API 和信令入口，根路径 `/` 返回 404。浏览器演示页面的 `/token`、
`/config` 开发服务没有部署到公网；业务客户端应通过账号 API 和 `/api/v1/room-admissions`
取得令牌及 `signaling_url`，然后连接返回的 WSS 地址。接口见
[会议控制与节点调度](../../docs/reference.md#会议控制与节点调度)。

默认关闭账号注册。测试新账号时可显式开启，已有账号和会话不会因切换开关被删除：

```sh
python3 deploy/cluster/deploy.py init --registration
python3 deploy/cluster/deploy.py up --recreate

# 完成测试账号准备后，按需要关闭公开注册。
python3 deploy/cluster/deploy.py init --no-registration
python3 deploy/cluster/deploy.py up --recreate
```

## 本机 Docker 调试

在尚未配置域名的新 runtime 中执行：

```sh
python3 deploy/cluster/deploy.py init
python3 deploy/cluster/deploy.py up --build
curl --fail http://127.0.0.1:18082/healthz
```

此时不启动 Caddy，API 位于 `127.0.0.1:18082`，信令位于
`ws://127.0.0.1:18180/webrtc` 和 `ws://127.0.0.1:18181/webrtc`。
只有本机客户端可使用这些地址。已有公网配置要恢复本机配置时，显式传入
`init --domain '' --public-ip 127.0.0.1`。

配置可以先在没有 Docker 的机器上生成。所有命令也支持把 `--runtime-dir PATH` 放在子命令之前，
例如 `python3 deploy/cluster/deploy.py --runtime-dir /opt/packetia/runtime init ...`。

## 日常操作

```sh
python3 deploy/cluster/deploy.py ps
python3 deploy/cluster/deploy.py logs --tail 100
python3 deploy/cluster/deploy.py logs --follow sfu-a
python3 deploy/cluster/deploy.py check

# 拉取代码后重建镜像，并重建容器加载最新程序和配置。
python3 deploy/cluster/deploy.py up --build --recreate

# 停止并删除容器，保留数据库与证书持久卷。
python3 deploy/cluster/deploy.py down
```

更新或停止 SFU 会中断该节点的现有会议；当前还没有会议排空和跨节点媒体迁移。
此版本为单控制服务和 SQLite 持久化，多个容器共用一台机器不提供机器故障容灾。
修改 `PACKETIA_PROJECT_NAME` 会切换到另一套容器和卷，正常更新应保持该名称稳定。

`check` 在容器内验证控制服务和两个 SFU 的 `/readyz`。SFU 的就绪状态依赖有效节点租约：
续租失败并过期后返回 503，进程存活探针 `/healthz` 仍可返回 200。
Docker 的 `restart: unless-stopped` 会重启退出的进程，单纯变成 `unhealthy` 不会自动重启容器。
公网 TLS、客户端 UDP 可达性和真实音视频收发仍需从外部客户端验证。

默认端口和路由如下：

| 用途 | 容器端口 | 宿主机暴露 |
| --- | --- | --- |
| HTTPS/WSS | edge 443/TCP | 公网 443/TCP |
| 证书申请与跳转 | edge 80/TCP | 公网 80/TCP |
| 控制 API | control 18082/TCP | 127.0.0.1:18082 |
| 注册与续租 | control 18083/TCP | 无 |
| SFU A/B 信令 | 18180、18181/TCP | 127.0.0.1 同端口 |
| SFU A/B 媒体 | 19100、19101/UDP | 公网同端口 |
| SFU A/B 管理与探针 | 19080、19081/TCP | 无 |
| SFU A/B 指标 | 18084、18085/TCP | 无 |

需要备份时先停止写入控制服务，再备份 `control-data` 卷及 runtime 配置。
部署工具的 `down` 不删除卷；不要用 `docker compose down -v` 代替正常停机。
证书申请失败先看 `logs edge`，节点不就绪先看 `logs sfu-a` / `logs sfu-b`；
通过 Docker 网络恢复控制服务后，SFU 可继续对同一实例续租。

## 验证方式与当前状态

配置生成及启动命令测试不依赖 Docker，在仓库根目录执行：

```sh
python3 scripts/test_cluster_deploy.py
```

原生双节点集成测试需要已编译的服务程序，例如：

```sh
python3 scripts/test_meeting_control.py \
  --control build/dev/server/PacketiaControl \
  --sfu build/dev/server/PacketiaSfu
```

集成测试会验证节点注册、会议管理、租约过期与恢复，以及 Control 重启后房间分配保持。
两项测试也已注册到启用测试和 WebRTC 依赖的 CMake/CTest 构建中。

本次已通过 8 项配置测试、原生双节点集成测试，以及 Compose 官方 CLI 对本地和公网两套配置的校验。
开发机没有 Docker 引擎，尚未执行镜像构建及容器启动；首次部署需在目标服务器上执行
`up --build`、`check`，并从外部客户端验证 HTTPS/WSS 和实际媒体收发。
