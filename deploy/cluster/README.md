# Docker 公网反向代理部署

Control 与 SFU 共用 `packetia-cluster:local` 镜像；Caddy 单独提供 HTTPS/WSS 入口。
只有公网 IPv4 时使用 Caddy 测试 CA；有域名时可自动申请公开证书。

```text
客户端 ── HTTPS :443 /api/* ─────────→ Caddy → Control :18082
       └─ WSS :443 /sfu-a/webrtc ───→ Caddy → SFU A :18180
       └─ WebRTC UDP :19100 ─────────────────→ SFU A
```

Caddy 自动处理 WebSocket 升级，`/sfu-a/webrtc` 转发为 SFU 内部的 `/webrtc`。
媒体 UDP 直接到 SFU，不经过 HTTP 代理。Control/SFU 的 HTTP 与信令端口仍只映射到宿主机回环地址；
节点注册、管理和指标端口不发布到公网。API 会话与入房 JWT 校验继续由后端执行。

## 各部署文件负责什么

| 文件 | 职责 |
| --- | --- |
| `Dockerfile` | 在构建阶段编译 `PacketiaControl` 与 `PacketiaSfu`，再复制程序和运行依赖到最终镜像 |
| `compose.yaml` | 定义 Control/SFU 的启动命令、环境变量、端口、挂载、健康检查和日志轮转 |
| `compose.public.yaml` | 与基础 Compose 文件合并，增加 Caddy 容器和公网入口 |
| `Caddyfile` | 配置证书、HTTP 跳转、HTTP API 和 WebSocket 代理 |
| `control.json`、`sfu-a.json`、`sfu-b.json` | `deploy.py init` 使用的配置模板 |
| `runtime/.env` | 生成的部署变量与持久密钥，由 Compose 的 `--env-file` 读取 |
| `runtime/*.json` | 生成后挂载进容器，供 C++ 程序实际读取的配置 |
| `deploy.py` | 生成配置并封装 Compose 的构建、启动、检查和日志命令 |

Control 与 SFU 使用同一个镜像，通过 `command` 分别运行不同的程序。
Compose 的 `services` 是容器定义；`x-runtime`、`x-jwt-environment` 是公共配置，
`&name` 定义 YAML 锚点，`<<: *name` 将公共字段合并进服务，不会额外启动容器。

常见配置的具体作用：

| 配置 | 含义 |
| --- | --- |
| `image` / `build` | 指定运行的镜像，以及需要构建时使用的源码目录和 Dockerfile |
| `command` | 覆盖镜像默认启动命令；SFU 执行 `PacketiaSfu /etc/packetia/sfu.json` |
| `environment` | 传入容器进程的环境变量，C++ 可通过 `getenv()` 读取 |
| `127.0.0.1:18180:18180/tcp` | 宿主机回环地址 18180 映射到 SFU 容器 18180，公网不能直接访问该宿主机端口 |
| `19100:19100/udp` | 宿主机 UDP 19100 映射到 SFU 容器 19100，媒体直连，需防火墙放行 |
| `8443:443/tcp` | 公网访问宿主机 8443，进入 Caddy 容器的 443；左右端口可以不同 |
| `./runtime/sfu-a.json:/etc/packetia/sfu.json:ro` | 将生成的宿主机配置只读挂载给 SFU；实际路径可由 `PACKETIA_CONFIG_DIR` 指定 |
| `control-data:/data` | 命名卷保存 Control 数据库；重建容器保留该卷即可保留数据 |
| `restart: unless-stopped` | 进程退出后自动尝试重启；手动停止后保持停止 |
| `healthcheck` | 定期执行就绪检查并标记容器健康状态；不健康本身不会触发自动重启 |
| `depends_on` + `service_healthy` | 启动时等待依赖服务健康，不保证依赖以后一直可用 |
| `logging: json-file` | Docker 保存容器标准输出/错误；当前按每文件约 10MB、最多 3 个文件轮转 |

Caddy 通过 Docker 默认网络内的服务名访问 `control:18082`、`sfu-a:18180`，
不需要经过宿主机的回环端口映射。JSON 中的 `0.0.0.0` 表示容器内的监听地址，
是否从公网可达还取决于宿主机端口映射、代理和防火墙。

`${PACKETIA_LOG_FORMAT:-json}` 表示优先使用部署变量，否则取默认值 `json`；
`${PACKETIA_JWT_SECRET:?Run deploy.py init first}` 表示变量为空或缺失时拒绝继续。
`.env` 用于变量替换，只有在 `environment` 等配置中引用后才会传给容器；
当前写死的 `PACKETIA_LOG_MODE: console` 不会被 `.env` 中的同名值覆盖。
`BUILD_JOBS` 是构建阶段的编译并行数，与运行时媒体线程数无关。

## 重新构建后端镜像

在仓库根目录执行：

```sh
docker build -f deploy/cluster/Dockerfile \
  -t packetia-cluster:local --build-arg BUILD_JOBS=2 .
```

构建阶段需要 FFmpeg 开发包，包括 `libswscale-dev`；运行阶段对应安装 `libswscale6`。
这些依赖由 Dockerfile 安装，无需在宿主机安装 FFmpeg。
构建前需要启动 Docker Desktop 或 Docker Engine。
Debian bookworm 的 libwebsockets 4.1 支持当前业务使用的标准请求头（含 `Authorization`），
但不支持枚举未知的自定义请求头；使用提供该 API 的新版库时，HTTP 模块会自动启用自定义请求头读取。

如果在 Apple Silicon Mac 上为 `x86_64` Ubuntu 服务器构建，明确指定目标架构：

```sh
docker buildx build --platform linux/amd64 --load \
  -f deploy/cluster/Dockerfile -t packetia-cluster:local \
  --build-arg BUILD_JOBS=2 .
```

服务器架构可以用 `uname -m` 查看；`aarch64` 对应 `linux/arm64`。

## 只有公网 IP，使用已有镜像

以下在 Ubuntu 服务器的仓库根目录执行。将 `203.0.113.10` 换成服务器公网 IPv4。
需要 Docker Engine、Compose V2+、Python 3，以及已加载的 `packetia-cluster:local` 镜像。

如果只调整代理和部署配置，上传更新后的 `deploy/cluster/` 配置和脚本即可，后端镜像无需重新编译。
涉及 C++ 代码或镜像依赖变更时，先构建并加载新镜像，再执行本节启动命令。
**保留服务器原有的 `runtime/` 目录与 `.env`**，不要用本地配置覆盖它们；
`init` 会保留已有 JWT 和节点密钥、注册开关，但会重新生成节点配置和公网信令地址。

```sh
python3 deploy/cluster/deploy.py init \
  --domain '' --public-ip 203.0.113.10 --tls-mode internal

python3 deploy/cluster/deploy.py up --single-sfu --recreate
python3 deploy/cluster/deploy.py check --single-sfu
```

`up` 复用已有后端镜像，只在缺少 Caddy 镜像时拉取它。新配置会重建选中的容器，现有连接会断开。
`--single-sfu` 启动 Control、SFU A、Caddy；它不会停止之前已经运行的 SFU B。
若无需 SFU B，在确认其没有正在服务的房间后，可以单独停止它。
省略 `--single-sfu` 则启动两个 SFU，SFU B 媒体端口为 `19101/udp`。

放行云防火墙/安全组和 Ubuntu UFW 的 `80/tcp`、`443/tcp`、`19100/udp`。
例如，已启用 UFW 时执行：

```sh
ufw allow 80/tcp
ufw allow 443/tcp
ufw allow 19100/udp
```

默认需要宿主机 80/443 空闲。若 443 已被 Xray 等服务占用，可将 Caddy 改为 8443：

```sh
python3 deploy/cluster/deploy.py init \
  --domain '' --public-ip 203.0.113.10 --tls-mode internal --https-port 8443
python3 deploy/cluster/deploy.py up --single-sfu --recreate
ufw allow 8443/tcp
curl -k --connect-timeout 3 --max-time 10 https://203.0.113.10:8443/healthz
```

云防火墙/安全组也需要放行 `8443/tcp`。该选项同时更新 Docker 端口映射、HTTP 跳转地址和两个 SFU 的 WSS 地址，
已有密钥继续保留。后续不带 `--https-port` 的 `init` 会沿用已配置端口；恢复默认端口时指定 `--https-port 443`。
容器内部仍监听 443，宿主机 443 上的原服务继续运行。
这只调整部署配置，无需重新编译后端镜像；更新服务器时需同时上传 `deploy.py`、`compose.public.yaml` 和 `Caddyfile`。

## 导出并信任测试证书

Caddy 启动后，在服务器执行以下命令，将 CA 根证书复制出来。
默认 Compose 项目名为 `packetia`；若修改过项目名，替换命令中的值。

```sh
docker compose --project-name packetia \
  --env-file deploy/cluster/runtime/.env \
  -f deploy/cluster/compose.yaml \
  -f deploy/cluster/compose.public.yaml \
  cp edge:/data/caddy/pki/authorities/local/root.crt /tmp/packetia-proxy-ca.crt
```

在测试电脑执行（替换服务器 IP）：

```sh
scp root@203.0.113.10:/tmp/packetia-proxy-ca.crt /tmp/packetia-proxy-ca.crt
curl --cacert /tmp/packetia-proxy-ca.crt https://203.0.113.10/healthz
```

如果配置了 8443，访问地址改为 `https://203.0.113.10:8443/healthz`，API 和 WSS 地址也需要带上这个端口。

macOS 浏览器还需要将根证书加入受信任钥匙串：

```sh
security add-trusted-cert -r trustRoot \
  -k "$HOME/Library/Keychains/login.keychain-db" /tmp/packetia-proxy-ca.crt
```

每台测试电脑都需要信任这个 CA；它与旧 `18443` 测试入口的 CA 不同。
`caddy-data` 卷保存 CA 和证书，更新部署时保留它。这里只复制 `root.crt` 公共证书。

## 地址与鉴权

| 公网入口 | 用途 |
| --- | --- |
| `https://203.0.113.10/healthz` | 匿名健康检查 |
| `https://203.0.113.10/api/v1/...` | 账号、会话、房间与入房许可 API |
| `wss://203.0.113.10/sfu-a/webrtc` | SFU A 信令，使用 Control 返回的地址和入房令牌 |
| `wss://203.0.113.10/sfu-b/webrtc` | 仅在 SFU B 运行时可用 |

这是后端 API/信令入口，**没有自带网站首页**，访问 `/` 返回 404。
登录和注册接口用于获取或建立身份；受保护的 API 要求会话 Bearer Token，入房仍要求有效 JWT。
新部署默认关闭账号注册，已有部署保留原开关；需要开放注册时使用 `init --registration` 后重建 Control。
JWT 签名密钥和节点密钥保留在后端，不发送给浏览器。

### 公网接口回归测试

可在测试电脑的仓库根目录运行标准库测试脚本，替换实际公网地址和端口：

```sh
python3 scripts/check_public_api.py --base-url https://203.0.113.10:8443 \
  --ca /tmp/packetia-proxy-ca.crt
```

未传 `--ca` 时跳过 TLS 证书校验，仅用于排查连通性，等同于 `curl -k`。
脚本要求已开放注册，会创建两个随机测试账号和一个独立房间，验证公开 HTTP 接口的正常请求、
鉴权与参数拒绝，以及 WSS 入会、重连、角色权限和移除通知。测试结束会结束房间并撤销会话；
没有删除账号接口，因此数据库中会保留随机账号记录。脚本不修改既有房间、不重启服务。
报告保存在 `examples/webrtc-room/artifacts/public-api-*/`，不保存密码或令牌。
该测试不覆盖真实音视频传输、弱网或容量；内部管理接口只验证公网路由隔离。
当前 `/api/v1/metrics` 可匿名读取，测试会记录这一行为。

旧 `compose.browser-test.yaml` 的 Node 页面是独立的 `18443` 测试入口。
本次 `init` 会将 SFU 对外信令地址切换为 `/sfu-a/webrtc`，不能直接与旧页面要求的 `/webrtc` 混用。
本配置不启动该测试页面，也不修改它的密码或证书；调用业务 API 的客户端应使用新返回的信令地址。

## 有域名时使用自动证书

先将域名 A 记录解析到服务器公网 IP，并放通 80/443，然后执行：

```sh
python3 deploy/cluster/deploy.py init \
  --domain rtc.example.com --public-ip 203.0.113.10 --tls-mode auto --https-port 443
python3 deploy/cluster/deploy.py up --single-sfu --recreate
curl https://rtc.example.com/healthz
```

Caddy 自动申请、续期公开证书，用户无需手动导入测试 CA。
如果两个 SFU 都要运行，启动和检查时省略 `--single-sfu`，并放通 `19101/udp`。

## 查看状态与日志

```sh
python3 deploy/cluster/deploy.py ps
python3 deploy/cluster/deploy.py logs edge control sfu-a --tail 100
python3 deploy/cluster/deploy.py check --single-sfu
```

`check` 检查后端就绪状态；Caddy 容器健康检查确认代理已加载配置。
公网 HTTPS、证书信任和 UDP 媒体连通性仍需从客户端验证。
健康接口成功不代表 WebRTC 已完成推流或订阅。

### HTTP 核心日志

后端通过现有异步日志器输出以下结构化事件，Docker 部署默认写到控制台：

- `http_server_started` / `http_server_stopped`：监听地址、端口与生命周期。
- `http_server_start_failed` / `http_server_service_failed` / `http_callback_failed`：关键运行失败。
- `http_request_completed`：请求响应已交给网络库，包含 `data.request_id`、`listen_port`、
  `method`、`path`、`status`、`duration_ms` 和请求/响应正文字节数。
- `http_request_failed`：请求在完成前中止，`reason` 区分超时、提前断开、无效响应、
  响应缓冲预算不足和写入失败等情况。尚未成功写出完整响应头时 `status` 为 `null`。

进入适配器的每个请求最多记录一次终态，长连接中的各个请求使用不同的 `request_id`；排查时结合服务、
进程和监听端口定位。`duration_ms` 使用单调时钟，从 HTTP 请求入口开始计算，包含接收正文、
业务排队、处理和响应写入；不代表客户端收到响应的时间，也不是媒体 RTT。
日志不记录查询参数、请求/响应正文、Authorization 或 Cookie。
普通请求使用 INFO，5xx 响应与传输故障使用 ERROR；成功的 `/healthz`、`/readyz` 使用 DEBUG。
该日志仅覆盖本 HTTP 适配器，不包含独立 WebSocket 信令监听器或媒体数据包。

```sh
docker logs --since 10m packetia-control-1 2>&1 | rg 'http_(request|server|callback)'
docker logs --since 10m packetia-sfu-a-1 2>&1 | rg 'http_(request|server|callback)'
```

服务器未安装 `rg` 时可改用 `grep -E`。此功能需要重新构建后端镜像并更新容器；
仅重启旧镜像不会加入这些日志。Caddy 的访问日志是独立配置，不受上述事件控制。

### 日志格式、文件输出与导出

`LOG_INFO()` 和 `GlobalLogger().WriteEvent()` 使用同一个异步日志器。
`PACKETIA_LOG_MODE=console|file|both` 控制输出位置，`PACKETIA_LOG_FORMAT=text|json` 控制格式，
`PACKETIA_LOG_LEVEL=debug|info|error` 控制最低级别。原生程序默认 `both`、`text`、`info`；
Compose 使用 `console`、`json`、`info`，Docker 将控制台输出保存到自己的轮转文件中。
应用自身文件默认为进程工作目录下的 `app.log`，可用 `PACKETIA_LOG_FILE` 指定路径；
需要应用文件输出时，修改 Compose 的 `PACKETIA_LOG_MODE` 并为日志目录配置持久挂载。

在 `runtime/.env` 修改以下内容后重建容器，可切换终端阅读格式，无需重新编译镜像：

```dotenv
PACKETIA_LOG_FORMAT=text
```

```sh
python3 deploy/cluster/deploy.py up --single-sfu --recreate
```

导出最近一小时的日志，在服务器仓库目录执行：

```sh
mkdir -p /tmp/packetia-log-export
docker logs --since 1h --timestamps packetia-control-1 > /tmp/packetia-log-export/control.log 2>&1
docker logs --since 1h --timestamps packetia-sfu-a-1 > /tmp/packetia-log-export/sfu-a.log 2>&1
docker logs --since 1h --timestamps packetia-edge-1 > /tmp/packetia-log-export/caddy.log 2>&1
tar -czf /tmp/packetia-log-export.tar.gz -C /tmp packetia-log-export
```

然后在测试电脑运行 `scp root@203.0.113.10:/tmp/packetia-log-export.tar.gz .` 下载。
容器名随 Compose 项目名变化，修改过项目名时相应替换；去掉 `--since 1h` 可导出所有尚未被轮转清理的日志。

## 修改后如何生效

| 修改内容 | 生效方式 |
| --- | --- |
| C++ 源码、Dockerfile、镜像依赖 | 重新构建镜像，上传并 `docker load`，然后重建后端容器 |
| `runtime/.env` 中的环境变量、端口映射、挂载 | 使用 `up --recreate` 重建容器；`docker restart` 沿用原环境和镜像 |
| 生成的 `runtime/*.json` | 当前程序启动时读取配置，重建相关容器；`init` 会按模板重新生成文件 |
| Caddyfile | 重载 Caddy，或重建 `edge`；不需要重新编译 C++ |

在构建机器上导出镜像：

```sh
docker save -o packetia-cluster.tar packetia-cluster:local
```

上传该镜像包后，在服务器执行：

```sh
docker load -i packetia-cluster.tar
python3 deploy/cluster/deploy.py up --single-sfu --recreate
```

`docker load` 更新镜像标签后，已有容器仍使用原来的镜像；需要重建容器才能切换。
`init` 只生成配置，`up` 才启动服务；同机增加容器也不会增加服务器本身的 CPU 或带宽。

### 公网 IP 访问出现 TLS internal error

IP URL 的客户端通常不发送 TLS SNI。经过 Docker 端口映射后，Caddy 看到的本地地址是容器地址，
不能据此选中公网 IP 证书。Caddyfile 通过全局 `default_sni {$PACKETIA_PUBLIC_HOST}` 指定默认的证书名称。
`curl -k` 只跳过客户端证书信任检查，无法解决服务器未选中证书造成的握手失败。

更新服务器的 `Caddyfile` 后，只重建代理容器即可：

```sh
docker compose --project-name packetia \
  --env-file deploy/cluster/runtime/.env \
  -f deploy/cluster/compose.yaml \
  -f deploy/cluster/compose.public.yaml \
  up -d --no-deps --force-recreate --wait --wait-timeout 60 edge
```

然后从客户端检查 `/healthz`，并按前面的步骤导入 CA 后去掉 `-k` 再验证证书。
无需重新构建后端镜像，保留现有 `caddy-data` 卷和 CA。

本机未启动 Docker daemon 时，也可先检查 Compose 配置（不会输出密钥）：

```sh
docker compose --project-name packetia \
  --env-file deploy/cluster/runtime/.env \
  -f deploy/cluster/compose.yaml \
  -f deploy/cluster/compose.public.yaml config --quiet
```
