## FFmpeg 与测试依赖

主程序和录像测试通过 `pkg-config` 查找系统安装的 libavformat、libavcodec 和 libavutil
开发包。无需编译 `third/ffmpeg` 下的源码，原有 `PACKETIA_FFMPEG_ROOT` 配置不再使用。
仅安装 `ffmpeg` 命令行程序不够，构建还需要对应的开发头文件和库。

Ubuntu 上安装依赖并构建主程序：

```sh
sudo apt update
sudo apt install build-essential cmake pkg-config libssl-dev \
  libavformat-dev libavcodec-dev libavutil-dev libsqlite3-dev

cmake -S . -B build -DBUILD_TESTING=OFF
cmake --build build --target Packetia -j"$(nproc)"
```

库必须与目标操作系统和 CPU 架构一致。可通过
`pkg-config --modversion libavformat libavcodec libavutil` 检查检测到的版本。

默认 `BUILD_TESTING=ON`，只有开启时才查找 GoogleTest 并生成测试目标。
Ubuntu 上运行完整默认测试：

```sh
sudo apt install libgtest-dev
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

录像集成测试额外需要 `-DPACKETIA_BUILD_RECORDING_TESTS=ON`，通过 PATH 查找 `ffmpeg`。
生成测试素材需要 libx264 和 AAC 编码支持，测试解码验证需要 Python 3，Ubuntu 可用 `sudo apt install ffmpeg python3` 安装。
录像测试也支持独立配置：

```sh
cmake -S service/RecordService/Test -B build/recording-tests
cmake --build build/recording-tests --parallel
ctest --test-dir build/recording-tests --output-on-failure
```

## 各路独立录像切片

各路独立 fMP4 切片、SQLite 索引及查询接口见[录制切片说明](service/RecordService/README.md)。
按 `(session_id, stream_id)` 分别录制，在关键帧处按媒体时间切段；支持逐流停止、重启，以及按会话、流和时间范围查询完成片段。构建需要 SQLite 3.24 以上开发库。

## 录像画面合成实验

可通过[双人合成示例](service/RecordService/examples/README.md)体验两路视频左右拼接和音频混音，
支持生成测试画面或输入两份已完成的录像，输出单路 H264/AAC MP4。

## 会议合成框架

新增 [ConferenceMixService](service/ConferenceMixService/README.md)，提供会议输入配置、独立工作线程、任务启停与编码输出接口。
当前尚未实现实际拼画面、混音和编解码。服务已注册到统一启停管理，默认关闭。

录制、AI、会议合成支持全局开关、流默认值与逐流覆盖配置，见[全局配置模块](config/README.md)和[服务配置与启停](service/README.md)。

## macOS 编译

需要 Xcode Command Line Tools（`xcode-select --install`）及 Homebrew。
在项目根目录执行：

```sh
brew install cmake ninja pkgconf googletest openssl@3 ffmpeg sqlite libwebsockets

cmake -S . -B build/macos -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH="$(brew --prefix)" \
  -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)" \
  -DPACKETIA_LIBWEBSOCKETS_ROOT="$(brew --prefix libwebsockets)"
cmake --build build/macos --parallel "$(sysctl -n hw.ncpu)"
ctest --test-dir build/macos --output-on-failure
```

只构建主程序可以在构建命令中加入 `--target Packetia`，产物为
`build/macos/Packetia`。完整构建也会生成测试程序；只构建主程序后不要直接运行全量 CTest。

macOS 使用 kqueue 事件调度，Linux 使用 epoll。依赖库必须与编译架构一致；
Apple Silicon 建议使用原生 arm64 Homebrew，避免混用 Rosetta 的 x86_64 库。
`third/libwebsocket` 中预编译的 Linux 库不用于 macOS。

服务默认监听 RTSP 554、SIP 5060、RTMP 1935、UDP 9000 和 WebSocket 8080，
启动前请确认这些端口可用。默认配置见 `server/ServerConfig.h`。

## 鉴权框架（待接入）

`service/Auth` 提供独立的 `packetia_auth` 库，目前只有类型、接口和组合入口，
没有 JWT、用户登录或房间权限策略实现，也尚未替换 WebRTC 现有共享 token 校验。

| 类型/接口 | 职责 |
| --- | --- |
| `AuthContext` | 验证后的身份、用途、Unix 秒有效期及权限列表 |
| `PermissionGrant` | 资源类型、资源 ID、允许的动作；不隐含通配或管理员权限 |
| `ResourceContext` | 服务端解析的资源 ID、所属房间/用户，及可选会话/流 ID |
| `IAuthenticator::Authenticate` | 具体实现验证签名、签发者、用途、有效期和凭证状态 |
| `IAccessPolicy::Check` | 具体实现检查用途与动作兼容性、授权范围、资源归属；缺少授权时拒绝 |
| `AuthService` | 构造时注入两个接口；缺少实现默认拒绝，并在认证/授权时检查身份和有效期 |

你可以从下面两项开始实现：

1. `JwtAuthenticator : IAuthenticator`：头文件已提供声明，你可以在 JwtAuthenticator.cpp 中实现 Authenticate。使用 jwt-cpp 等成熟库，成功时返回 `AuthResult::Success(context)`，失败时返回错误码。必须要求有效期，不能只 decode 后就返回成功。
2. `RoomAccessPolicy : IAccessPolicy`：检查目标房间、轨道的实际归属，以及 JoinRoom/Publish/Subscribe 等权限；同时拒绝 WebRTC 凭证执行管理操作。

依赖注入与调用示例（authenticator/policy 为你实现的对象）：

```cpp
#include "service/Auth/AuthService.h"

using namespace service::auth;
AuthService auth(authenticator, policy);
const auto result = auth.Authenticate(token, Audience::WebRtc);
if (!result.Succeeded()) { /* 返回认证错误 */ }
else
{
    const auto decision = auth.Authorize(*result.context, Action::JoinRoom, resource);
    if (!decision.Allowed()) { /* 返回权限错误 */ }
    else { /* 执行业务操作 */ }
}
```

后续由 ServerApp 创建并注入共享 AuthService，WebRtcService 绑定成功认证的上下文到连接，
RPC 入口按调用认证；在资源分配或业务变更前执行授权。ResourceContext 的归属信息来自服务端状态，
不能直接复制客户端提交的房间/用户字段。鉴权组件和注入的时钟可能被并发调用，具体实现需保证线程安全。

这里的同步入口不管理连接、定时器或撤销列表：过期断开、媒体清理、凭证刷新和撤销由后续接入实现。
仅在信令时检查有效期不会自动终止已有媒体转发。JWT 的有效期为绝对 Unix 秒，不能使用单调时钟。
错误详情留给内部处理，不记录或回显 token。TURN/ICE 凭证保持各自协议边界。

## RTP 协议规范

本节整理项目涉及的主要 RTP 协议规范，便于读代码和继续实现。字段和处理要求以
RFC 正文及勘误为准；规范要求与当前支持范围分别说明，不代表项目已完整实现所有 RFC。
RTP/RTCP 负责媒体封装、来源、排序、时间与反馈，SDP 负责协商格式，ICE/TURN 负责连通与中继。

### 主要 RFC

| 功能 | 规范 | 重点 |
| --- | --- | --- |
| RTP/RTCP 基础 | [RFC 3550](https://www.rfc-editor.org/rfc/rfc3550) | 固定头、报告、同步、SSRC 管理、接收统计 |
| 音视频 profile | [RFC 3551](https://www.rfc-editor.org/rfc/rfc3551) | 静态/动态 PT、时钟、marker |
| 通用扩展头 | [RFC 8285](https://www.rfc-editor.org/rfc/rfc8285) | 单字节/双字节格式、extmap、混合格式；取代 RFC 5285 |
| SDP 与 Offer/Answer | [RFC 8866](https://www.rfc-editor.org/rfc/rfc8866)、[RFC 3264](https://www.rfc-editor.org/rfc/rfc3264) | rtpmap、fmtp、方向和能力交集 |
| RTP/RTCP 同端口 | [RFC 5761](https://www.rfc-editor.org/rfc/rfc5761) | rtcp-mux、PT 冲突限制 |
| BUNDLE/MID | [RFC 8843](https://www.rfc-editor.org/rfc/rfc8843) | 多媒体段共用传输及分流 |
| RID/simulcast | [RFC 8852](https://www.rfc-editor.org/rfc/rfc8852)、[RFC 8853](https://www.rfc-editor.org/rfc/rfc8853) | 编码流标识与多编码协商 |
| 反馈 | [RFC 4585](https://www.rfc-editor.org/rfc/rfc4585)、[RFC 5104](https://www.rfc-editor.org/rfc/rfc5104) | AVPF、NACK、PLI、FIR |
| RTX | [RFC 4588](https://www.rfc-editor.org/rfc/rfc4588) | OSN、apt、独立重传序列号空间 |
| Reduced-size RTCP | [RFC 5506](https://www.rfc-editor.org/rfc/rfc5506) | 独立反馈包与周期性 compound RTCP |
| 扩展报告/CNAME | [RFC 3611](https://www.rfc-editor.org/rfc/rfc3611)、[RFC 7022](https://www.rfc-editor.org/rfc/rfc7022) | XR、跨流来源关联 |
| 拥塞反馈与停止条件 | [RFC 8888](https://www.rfc-editor.org/rfc/rfc8888)、[RFC 8083](https://www.rfc-editor.org/rfc/rfc8083) | 标准拥塞反馈、circuit breaker |
| SRTP/DTLS-SRTP | [RFC 3711](https://www.rfc-editor.org/rfc/rfc3711)、[RFC 7714](https://www.rfc-editor.org/rfc/rfc7714)、[RFC 5764](https://www.rfc-editor.org/rfc/rfc5764) | 认证、加密、重放防护与密钥协商 |
| WebRTC 分流与媒体要求 | [RFC 7983](https://www.rfc-editor.org/rfc/rfc7983)、[RFC 8834](https://www.rfc-editor.org/rfc/rfc8834) | STUN/DTLS/RTP/RTCP 分流 |
| 冗余/FEC | [RFC 2198](https://www.rfc-editor.org/rfc/rfc2198)、[RFC 5109](https://www.rfc-editor.org/rfc/rfc5109)、[RFC 8627](https://www.rfc-editor.org/rfc/rfc8627) | RED、ULPFEC、FlexFEC |

### RTP 基础头：RFC 3550 §5.1

```text
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|V=2|P|X|  CC   |M|     PT      |       sequence number         |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                           timestamp                           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|           synchronization source (SSRC) identifier            |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|               CSRC identifiers (0..15 entries)                 |
|                              ...                              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|       extension envelope and data, present only if X=1        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                   payload and optional padding                |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

多字节整数采用网络字节序，即大端；固定头 12 字节，CSRC、扩展、载荷及尾部填充不在该长度内。
`#pragma pack` 只控制内存布局，不能代替字节序转换，也不能保证非对齐访问安全。

| 字段 | 位数 | 含义与约束 |
| --- | --- | --- |
| V | 2 | RTP 版本为 2，与 SDP 的 v=0 无关 |
| P | 1 | 存在尾部填充；最后一字节表示填充总长度，包括自身，不得为 0 |
| X | 1 | CSRC 后有一个扩展封套，封套内可有多个元素 |
| CC | 4 | CSRC 个数，0..15，每项 4 字节 |
| M | 1 | 含义由 profile/载荷规范定义，并非统一关键帧标志 |
| PT | 7 | 载荷格式映射，不是全局轨道标识 |
| sequence | 16 | 每个原始包加 1，按 65536 回绕，初始值应随机 |
| timestamp | 32 | 采样时刻，按载荷时钟计数及 2^32 回绕，初始值应随机 |
| SSRC | 32 | 会话内同步源标识，随机选择并处理碰撞 |
| CSRC | 每项 32 | 混流器输出的贡献源列表，不能当作 MID |

- 常用静态 PT：PCMU=0、PCMA=8；96..127 为常用动态区间。H264=96、Opus=111 是本地能力示例，实际收发使用 SDP 协商值。
- rtcp-mux 模式下不能使用 PT 64..95，以免和 RTCP 类型冲突；分流后仍需校验长度与结构。
- 同一视频帧的多个包通常时间戳相同、序列号不同。序列号比较需处理回绕、乱序、重复和丢包，不能直接比较普通整数大小。
- H264/H265 通常采用 90000 Hz 时钟；30 fps 相邻采样帧通常相差 3000。Opus RTP 时钟固定为 48000 Hz，20 ms 相差 960。
- 时间戳按采样时间产生，不是收包时间；传输顺序不同时可能不单调。音频 DTX 不发包时，媒体时钟仍推进。
- SSRC、MID、RID、PT 分别标识来源、媒体段、编码流、载荷格式。不同流的时间戳不能直接比较，需通过 RTCP SR 的 NTP/RTP 对和 CNAME 建立同步关系。

### 扩展封套与元素：RFC 3550 §5.3.1 / RFC 8285

```text
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|       defined by profile      |       length (32-bit words)   |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                 extension data and alignment padding          |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

One-byte, profile=0xBEDE:
+-+-+-+-+-+-+-+-+-------------------------------+
|  ID   |  len  |       data (len + 1 bytes)      |
+-+-+-+-+-+-+-+-+-------------------------------+

Two-byte, profile=0x1000 | appbits:
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-------------------------------+
|       ID      |     length    |       data (length bytes)      |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-------------------------------+
```

封套在 `12 + 4 * CC` 字节处。length 是扩展数据的 32 位字数，不含封套自身 4 字节，允许为 0。
未知 profile 按封套长度跳过；元素逐字节解析，整个扩展数据区用零字节补至 4 字节边界。

- 单字节 ID=1..14，len=0..15，实际数据 1..16 字节；ID=15 时忽略 len 并停止元素解析。
- RFC 8285 §4.1.2 还要求遇到 ID=0 且 len 非零时停止解析。
- 双字节 ID=1..255，包括 ID=15；length 直接表示 0..255 字节，不加 1。profile 低 4 位 appbits 未约定时发送端应置零，接收端忽略。
- 两种格式中的 0x00 都只占一个 padding 字节；双字节格式不再读取其后的 length。
- 同一包只采用一种元素格式。流内混用两种格式需通过 extmap-allow-mixed 或带外方式确认接收方支持。
- 扩展 ID 是 SDP extmap 协商的局部编号，URI 才标识含义；遵守收发方向及 BUNDLE 映射一致性。下游转发按订阅者的编号重建扩展。
- 自定义“私有头”应约定扩展 URI 和数据编码，不改变固定头或占用保留 ID，也不直接发送 C++ 结构体内存。

```sdp
a=extmap:1 urn:ietf:params:rtp-hdrext:sdes:mid
a=extmap:3 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01
```

上例的 1 和 3 不是标准固定编号。常见扩展还包括 RID/repaired RID，以及
[RFC 6464](https://www.rfc-editor.org/rfc/rfc6464) 的音量扩展。
当前 TWCC 使用上例草案格式，与 RFC 8888 的拥塞反馈不是同一种线格式。

```text
base_header_bytes = 12 + 4 * CC
extension_bytes   = X ? 4 + 4 * extension_length_words : 0
payload_offset    = base_header_bytes + extension_bytes
padding_bytes     = P ? packet[last] : 0
payload_bytes     = packet_bytes - payload_offset - padding_bytes
```

每一步必须先校验长度。尾部填充不能侵入头部/扩展。RTP 尾部填充、扩展对齐填充、SRTP
认证标签/MKI 是不同概念；上述公式仅用于去除 SRTP 尾部保护数据后的明文 RTP。

### 常见媒体载荷

载荷头位于 RTP 头之后，与 RTP 扩展头分开解析。

| 编码 | 规范 | 主要内容 |
| --- | --- | --- |
| H264 | [RFC 6184](https://www.rfc-editor.org/rfc/rfc6184) | Single NAL、STAP-A、FU-A；packetization-mode/profile-level-id 协商 |
| H265 | [RFC 7798](https://www.rfc-editor.org/rfc/rfc7798) | 单 NAL、AP、FU、PACI；DONL/DOND 取决于协商 |
| Opus | [RFC 7587](https://www.rfc-editor.org/rfc/rfc7587) | 一个 RTP 载荷携带一个 Opus packet；TOC/帧结构另见 RFC 6716 |
| PCMU/PCMA | RFC 3551 | 常见单声道 8 kHz；marker 可表示 talkspurt 开始 |
| AAC MPEG4-GENERIC | [RFC 3640](https://www.rfc-editor.org/rfc/rfc3640) | AU-headers-length 以位计；字段长度由 fmtp 约定 |
| AAC MP4A-LATM | [RFC 6416](https://www.rfc-editor.org/rfc/rfc6416) | LATM 格式，不能按 MPEG4-GENERIC 解析 |
| VP8 | [RFC 7741](https://www.rfc-editor.org/rfc/rfc7741) | 载荷描述符、分区、PictureID 与时间层 |

```text
Single NAL: [F:1 | NRI:2 | Type:5] [NAL data ...]     Type=1..23
STAP-A:     [F:1 | NRI:2 | Type=24]
            [NAL size:16] [complete NAL ...]          repeated
FU-A:       [F:1 | NRI:2 | Type=28]
            [S:1 | E:1 | R:1 | original Type:5] [fragment ...]
```

FU-A 的 S/E 表示 NAL 分片开始/结束，不能同时为 1，R 为保留位。重组恢复原 NAL 头并校验
序列连续性，不能把缺少分片的 NAL 输出为完整数据。H264 的 M 表示 access unit 最后一个包，
FU 的 E 表示某个 NAL 的结束，两者不同。RTP 载荷不包含 Annex-B 起始码，由解包输出时添加。

### RTCP、同步和接收质量

```text
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|V=2|P| count/FMT | packet type   |           length              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    packet-specific body                       |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

RTCP length 表示整个单包的 32 位字数减 1，包含公共头，字节数为 `(length + 1) * 4`；
与 RTP 扩展的 length 定义不同。数据报可能包含多个 RTCP 单包，要逐个校验。

| PT | 类型 | 内容 |
| --- | --- | --- |
| 200 | SR | SSRC、NTP/RTP 时间对、累计发包/载荷字节数和报告块 |
| 201 | RR | 报告者 SSRC 和接收报告块 |
| 202 | SDES | CNAME 等来源描述 |
| 203 | BYE | 来源离开及可选原因 |
| 204 | APP | 应用自定义内容 |
| 205 | RTPFB | 如 FMT=1 的 Generic NACK |
| 206 | PSFB | 如 FMT=1 PLI、FMT=4 FIR |
| 207 | XR | 扩展报告 |

每个接收报告块 24 字节：SSRC(32)、fraction lost(8)、cumulative lost(24 signed)、
extended highest seq(32)、jitter(32)、LSR(32)、DLSR(32)。累计丢包是有符号 24 位值。
LSR 为最近 SR 的 NTP 中间 32 位，DLSR 单位为 1/65536 秒。

```text
transit_i = arrival_i_in_RTP_clock_units - RTP_timestamp_i
D         = transit_i - transit_(i-1)
J         = J + (abs(D) - J) / 16
RTT       = A - LSR - DLSR
```

J 的单位为 RTP clock tick，不直接是毫秒。A 是当前 NTP 中间 32 位；RTT 计算需处理回绕，
没有可用 LSR 时不能使用此公式。不同 SSRC 的时钟和统计不能混算。
基础 compound RTCP 通常以 SR/RR 开始并包含 SDES CNAME。发送间隔按成员数、带宽、随机化
及 reconsideration 计算；AVPF 提前反馈、RFC 5506 独立反馈包也有调度及周期性报告要求。

### 重传、冗余与 SFU 转发

Generic NACK 的每项 FCI 为 PID(16)+BLP(16)：PID 是第一个丢失序列号，BLP 第 i 位表示
`(PID + i + 1) mod 65536` 也丢失。实现需处理回绕、重复反馈、缓存过期和重传限频。
PLI 没有 FCI，表示图片丢失，不列出具体包，也不保证立即收到 IDR。
FIR 的 FCI 包含目标 SSRC 和请求序号，不能按 PLI 解析；发布者/编码器需实际响应请求。

RTX 通过 apt 关联原 PT，采用独立重传序列号，载荷开头为 16 位 OSN；SSRC 复用模式
使用独立 RTX SSRC 并通过 FID 分组关联。直接重发原包与 RFC 4588 RTX 封装是不同方案。
原包重发保留 RTP sequence，但每次网络发送分配新的 TWCC sequence；同时考虑 SRTP 重放窗口。
RED、ULPFEC、FlexFEC 分别协商并实现恢复；Opus useinbandfec 不代表通用 RTP FEC 支持。

SFU 应先分流并认证/解密 SRTP，再解析 RTP；不能让未认证的扩展修改轨道绑定。
BUNDLE 内按协商 MID 和绑定 SSRC 分流，下游按订阅者协商值改写 PT、MID、SSRC 和传输序号，
相关 RTCP 也需正确映射。接收/发送统计按来源维护，TWCC 使用传输级序列号空间。
MTU 扣除 IP/UDP、变长 RTP 头、SRTP 和 TURN 开销；固定 payload 上限不是 RFC 通用要求。

### 当前实现范围

| 入口 | 当前能力与边界 |
| --- | --- |
| [Rtp.h](Rtsp/Rtp.h)、[Rtp.cpp](Rtsp/Rtp.cpp) | 固定头与接收统计；完整 SSRC 碰撞管理和 RTCP 调度仍需核对 |
| [RtpReceiver.cpp](Rtsp/RtpReceiver.cpp) | 版本/PT/SSRC 校验，按 CC/X/P 计算载荷位置 |
| [RtpHeaderExtensions.h](Rtsp/Rtp/RtpHeaderExtensions.h) | 单/双字节解析，未知 profile 跳过；重复 ID 按本地策略拒绝；ID=0 且 len 非零的停止规则待补，失败可能留下部分输出 |
| [RtpTransportCcExtension.cpp](Rtsp/RtpTransportCcExtension.cpp) | 仅单字节 TWCC、ID 1..14、两字节序号 |
| [RtpSenderTrack.cpp](Rtsp/RtpSenderTrack.cpp) | 下游改写、缓存、NACK 原包重发；MID/TWCC 输出为单字节，尚非完整 RTX |
| [H264RtpPayloadParser.cpp](Common/Depacketizer/H264RtpPayloadParser.cpp) | Single NAL/STAP-A/FU-A，不代表全部 H264 打包模式 |
| [AudioRtpDepacketizerFactory.cpp](Common/Depacketizer/AudioRtpDepacketizerFactory.cpp) | G.711/Opus 接收工厂，其他路径/编码需单独核对 |
| [RtcpReciver.cpp](protocol/Rtcp/RtcpReciver.cpp) | SR/RR/BYE、NACK/PLI/FIR、传输反馈入口；当前跳过 SDES，枚举不代表处理器已实现 |
| [SdpNegotiator.cpp](src/sdp/SdpNegotiator.cpp) | PT/codec、方向、BUNDLE、扩展协商；WebRTC 使用 UDP DTLS-SRTP 与 rtcp-mux |

后续按扩展边界处理、SDES/CNAME 与 compound RTCP、RTX、RID/simulcast、拥塞反馈、FEC
逐项推进，验证协商、实际收发与退出清理。

## TURN UDP 中继

`protocol/turn` 提供 TURN 模块，由 `ServerApp` 在 `Packetia` 主进程内统一启动、关闭，
与 WebRTC 直接组合，不需要 RPC 或另外启动 TURN 进程。支持 IPv4/IPv6 UDP 控制与中继套接字。
支持长期凭证认证、401/438 challenge、Allocate/Refresh、CreatePermission、
Send/Data indication、ChannelBind/ChannelData、生命周期清理、配额与 peer ACL。
当前不包含 TCP/TLS 或独立 STUN Binding 服务，不能等同于 coturn 全功能。
默认关闭，在配置文件中设置 `turn.enabled=true`，或通过 `PACKETIA_TURN=true` 开启。
账号由 `PACKETIA_TURN_USER` 和 `PACKETIA_TURN_PASSWORD` 环境变量提供，要求非空可打印 ASCII。
TURN 开启后，缺少账号、绑定失败或后续模块启动失败都会触发启动回滚。
网络参数与配额放在 JSON 的 `turn` 节，字段见 `config/packetia.example.json` 和 `config/AppConfig.h`。

```sh
cmake --build build/webrtc-integration --target Packetia
export PACKETIA_CONFIG="$PWD/config/packetia.example.json"
export PACKETIA_TURN=true
export PACKETIA_TURN_USER=packetia-test
export PACKETIA_TURN_PASSWORD=local-test-password
export PACKETIA_WEBRTC=true
export PACKETIA_WEBRTC_PUBLIC_IP=127.0.0.1
export PACKETIA_WEBRTC_TOKEN=local-test-token
build/webrtc-integration/Packetia
```

公网运行需配置监听/中继/公告 IP 与 relay 端口范围，并放通对应 UDP 端口。
默认 peer ACL 拒绝私网、回环、链路本地及组播目标；配置 `turn.local_test=true` 仅允许回环 peer。
控制监听与 SFU 媒体仍使用不同的 UDP 端口，浏览器仍需在 `iceServers` 中配置 TURN URL 和账号。
启动先完成 I/O 和工作线程，再启动 TURN 与网络模块；关闭时先停止 WebRTC，最后停止 TURN 和 I/O。
`PacketiaTurn` 入口保留作独立协议调试工具，正常运行使用上面的 `Packetia` 入口即可。

### TURN 双栈与 IPv6 中继

默认仍使用 IPv4。在 JSON 中设置 `turn.dual_stack=true`、`turn.listen_ip="::"` 后，一个 IPv6 控制 socket
同时接收 IPv4/IPv6 客户端；接收的 IPv4-mapped 地址统一为 IPv4，回复时由 socket 层转回映射形式。
不启用双栈的 IPv6 监听设置 `IPV6_V6ONLY=1`。

`turn.relay_bind_ip` / `turn.advertised_ip` 配置 IPv4 中继绑定/公告地址，
`turn.relay_bind_ip_v6` / `turn.advertised_ip_v6` 配置 IPv6 中继绑定/公告地址。两组配置独立，
IPv6 中继默认关闭；程序接口中清空某组的两个地址可关闭该地址族。
客户端未携带 `REQUESTED-ADDRESS-FAMILY` 时仍请求 IPv4，值 `0x02` 请求 IPv6。
未配置所请求的地址族返回 440，对端地址族与中继不匹配返回 443。
IPv4 客户端可申请 IPv6 中继，IPv6 客户端也可申请 IPv4 中继。
每个 Allocation 当前只分配一种地址族，不支持 `ADDITIONAL-ADDRESS-FAMILY` 双重分配。

本机双栈测试配置，将其作为配置文件的 `turn` 节并使用上述主程序入口：

```json
"turn": {
  "enabled": true,
  "listen_ip": "::",
  "listen_port": 3478,
  "dual_stack": true,
  "relay_bind_ip": "127.0.0.1",
  "advertised_ip": "127.0.0.1",
  "relay_bind_ip_v6": "::1",
  "advertised_ip_v6": "::1",
  "local_test": true
}
```

公网部署应改为实际接口及可达的公告地址，并放通 IPv4/IPv6 的控制端口和中继端口范围。
IPv6 默认 ACL 同样拒绝回环、ULA、链路本地、组播及映射/兼容 IPv4 地址。
当前仅接受无 zone ID 的数字 IP；不支持携带 scope 的链路本地对端。
TURN 测试覆盖四种接入/中继地址族组合的真实 UDP 转发、通道、权限到期和资源释放。
SFU 的双栈监听与双地址 SDP 候选发布尚未在这一功能中实现。

## 双浏览器房间与 TURN 示例

`examples/webrtc-room` 使用实际 Room/SfuEndpoint 转发链路：两个浏览器加入同一房间，
A 发布，B 订阅，并可在 B 建连前勾选 Force TURN 强制 `iceTransportPolicy=relay`。
WebSocket 信令支持 join、tracks、publish、subscribe、leave；每个连接当前只承担发布或订阅一种角色，
房间内媒体重协商尚未开放。订阅通过轨道 ID 和下游 MID 显式绑定。

需要启用项目的 WebRTC/libSRTP 构建依赖，并安装 Node.js。按已有构建目录运行：

```sh
cmake --build build/webrtc-integration --target PacketiaRoomDemo
cd examples/webrtc-room
npm ci
npm start
```

打开 `http://127.0.0.1:18000`。启动器生成临时 token/TURN 凭证并启动 C++ 示例；
默认 WebSocket 18080、SFU UDP 19000、TURN UDP 13478，均用于本机演示。
其他构建目录通过 `PACKETIA_ROOM_DEMO_BINARY` 指定二进制。
示例 TURN 只允许转发至该 SFU 的回环媒体地址。

启动示例后运行 `npm run check`，需要本机 Google Chrome；检查两个独立浏览器中的视频帧增长、
音频流量、订阅端选中的 relay 候选、视频尺寸和退出清理，并在忽略的 artifacts 目录输出截图。

## WebRTC SDP 协商

房间发布/订阅的运行入口与限制见上面的双浏览器示例。

- 新增 SDP codec 构建、参数校验和协商逻辑。
- 实现 Offer/Answer 状态管理、提交与回滚机制。
- 支持媒体方向、RTCP feedback 和 RTP header extension 能力协商。
- 完善 BUNDLE、ICE、DTLS、SSRC 与 m-line 校验。
- 抽取通用 ASCII 字符串处理工具。
- 补充 SDP codec 和 negotiator 边界测试。

## Version 0.2.0 – 2025-12-19

### Highlights
This release refactors RTSP over TCP handling by separating RTP/RTCP interleaved
media packets from RTSP control message parsing, significantly improving
stability and correctness.

### Improvements
- Demultiplex RTP/RTCP interleaved packets before RTSP parsing
- Correctly handle TCP sticky packets and fragmented interleaved frames
- Avoid treating empty buffers and partial packets as connection errors

### Bug Fixes
- Fix RTSP parser being disrupted by interleaved RTP packets
- Fix incorrect connection termination on empty read buffer

### Design Changes
- Introduce a clear separation between media data plane and RTSP control plane
- RTP/RTCP packets are now consumed at the connection read level

# Version 0.3.0 – 2025-12-30
## Highlights

This release completes the RTSP-over-TCP interleaved media pipeline by
introducing explicit channel-to-track binding and a codec-aware RTP track
factory, enabling correct RTP/RTCP demultiplexing and per-track packet handling.

## Improvements

- Introduce RtpInterleaved channel map to dispatch interleaved RTP/RTCP packets
by negotiated TCP channel

- Bind RTP/RTCP interleaved channels to tracks during SETUP negotiation

- Add createTrack() factory to construct codec-specific RTP tracks

- Parse a=control:streamid=N safely via a dedicated ParseStreamId() helper

- Improve RTP debug visibility with optional per-packet header logging

- Generate compile_commands.json via CMake for accurate IDE tooling support

## Bug Fixes

- Fix incorrect use of track index as interleaved channel ID

- Fix RTP/RTCP packets being routed without validated SETUP binding

- Fix potential session/track mismatch during multi-track SETUP sequences

- Fix undefined reference caused by missing function definitions at link time

## Design Changes

- Clearly separate RTSP control plane (request/response parsing)
from media data plane (RTP/RTCP packet dispatch)

- RtpTrack lifetime is now managed via shared_ptr, while interleaved dispatch
uses weak_ptr to avoid ownership cycles

- TCP interleaved channels are treated as connection-scoped identifiers,
independent of media track indices


# Version 0.2.1 – 2026-01-01
## Highlights

- This release fixes RTSP RECORD failures caused by missing track registration during SETUP, and introduces an initial - - RTP-over-TCP interleaved processing pipeline that decouples network I/O from media processing.

## Improvements

- Register SDP tracks into MediaSession::tracks_ during SETUP to ensure RECORD operates on a fully initialized session

- Add MediaSession::BindRtpTrack(trackIdx, track_ptr) to associate trackIdx -> RtpTrack for deterministic lookup during - - RECORD and media processing

- Introduce an initial worker-based RTP input pipeline (PacketPool + WorkerPool) to offload heavy RTP processing from the connection read thread

- Add structured logging for SETUP track matching (control/codec/pt/clock/trackIdx) and for RECORD session/track validation

## Bug Fixes

- Fix RTSP RECORD returning no response when MediaSession::tracks_ is empty (root cause: SETUP did not register tracks)

- Fix incorrect/misleading logs in RECORD handler (e.g., printing “SETUP request” in RECORD path)

# Version 0.3.0 – 2026-01-02
## Highlights

- This release introduces a reusable real-time packet delivery framework for RTP/RTCP processing. It adds a fixed-capacity PacketPool for deterministic memory management and a dedicated SPSC RtpRingBuffer (with RTCP priority) to decouple I/O from media processing, improving stability under load.

## Improvements

- Add PacketPool (object/memory pool) to provide fixed-capacity packet buffers and avoid frequent heap allocations in the RTP data path

- Add RtpRingBuffer built on SPSC rings, supporting separate RTP/RTCP queues with RTCP-first dequeue policy

- Refactor interleaved RTP ingestion to a “copy + enqueue” model, simplifying the read thread responsibilities and reducing lifetime hazards

- Introduce queue/pool observability hooks (queue depth, dropped/exhausted counters) to facilitate load testing and tuning

## Bug Fixes

- Fix potential lifetime issues when dispatching interleaved RTP/RTCP across threads by avoiding raw pointer ownership leaks and centralizing buffer release

- Resolve const-correct locking issue in PacketPool by making internal mutex mutable (enables thread-safe const observers such as size()/stats())


# Version 0.3.1 – 2026-01-06
## refactor(network): fix connection lifetime issues and introduce factory-based initialization

- Introduce two-phase initialization for TcpConnection (construct + Start)
- Move channel registration and event enabling out of constructors
- Add factory method RtspConnection::Create to enforce correct init order
- Replace raw `this` captures in callbacks with weak_ptr to prevent UAF
- Eliminate bad_weak_ptr caused by shared_from_this in constructors
- Unify RTSP connection creation through server OnConnect factory path
- Improve disconnect handling to avoid delayed callback accessing destroyed server


# Version 0.3.2 – 2026-01-07


## Architecture Changes

- Introduce a generic ShardedWorkerPool with key-based sharding to guarantee
per-key ordering while allowing parallel execution across workers.

- Add a pluggable IJobHandler interface, enabling different subsystems
(RTP, RTCP, media processing, future non-RTSP tasks) to reuse the same worker pool.

- Refactor RTP processing into a clear producer → dispatcher → consumer model:

- IO thread: framing and minimal copy only

- Worker threads: RTP/RTCP consumption and track-level processing

- Decouple RTP consumption logic from RTSP, making the worker service reusable
by other modules.

## RTP / RTSP Improvements

- Implement a clean RTSP over TCP interleaved handling model:

- Strict $ <channel> <length> <payload> framing

- Robust handling of sticky packets and fragmented frames

- Add explicit channel → track binding established during RTSP SETUP.

- Ensure RTP/RTCP packets are never parsed by the RTSP control parser.

- Move all heavy RTP processing out of IO threads to worker threads.

## Worker & Scheduling

- Add queue depth limits and drop policies (DropHead / DropTail) to protect
the system under load.

- Guarantee in-order processing for the same track_id via consistent sharding.

- Provide safe shutdown semantics with optional queue draining.

- Improve statistics collection for enqueue, dequeue, drops, and max queue depth.

## Memory & Stability

- Centralize packet lifetime management via PacketPool.

- Ensure all dropped or unbound jobs properly release packet memory.

- Eliminate potential memory leaks when queues overflow or tracks are unbound.

- Improve defensive checks for invalid channels and oversized RTP packets.

# Version 0.3.3 – 2026-01-11
## fix(workerpool): fix PacketPool leak by restoring job-based release chain

Fix a critical memory leak where PacketPool objects were never released
after being consumed by ShardedWorkerPool workers.

Root cause:
- Packet ownership was lost during job dispatch.
- job.deleter() was invoked, but Packet::owner was null, so PacketPool::release()
  was never called.
- Additionally, job drop paths (queue full, post failure, DropHead) were
  not releasing payloads, causing pool exhaustion.

This patch:
- Restores a strict ownership chain: Packet -> WorkJob -> deleter -> PacketPool
- Ensures all failure / drop paths in ShardedWorkerPool::post() call job.deleter()
- Guarantees that every Packet acquired from PacketPool is eventually released
  exactly once.

After this change:
- PacketPool stats (acquired / released) remain balanced under load
- PacketPool no longer exhausts under sustained RTP traffic
- WorkerPool becomes memory-safe under backpressure and drops

This fixes frequent RTSP/RTP failures (-12) caused by pool exhaustion.


# Version 0.3.3 - 2026-01-13
## feature: implement RTSP TCP interleaved demux and RTP worker dispatch

- Add RTSP interleaved ($) frame parsing in RtspConnection
- Bind interleaved channel to RtpTrack during SETUP (RTP/RTCP)
- Deliver interleaved RTP/RTCP payloads to media WorkerPool
- Use track-based key for per-track serial processing
- Integrate PacketPool for RTP payload buffering

tips:I'm tired today, going to rest and then continue studying BBR and Gerrit
This establishes the complete TCP → channel → track → worker RTP pipeline.


# Version 0.3.4 - 2026-01-14
## fix: optimize RTP track map locking and reduce log noise

- Replace std::mutex with std::shared_mutex for RtpJobHandler track map
  to improve concurrency under high RTP load (read-heavy scenario)
- Use shared_lock for track lookup and unique_lock for track insert/erase
- Downgrade excessive ERROR logs to WARN for normal track teardown and race cases
- Improve log context to include key and payload length for easier troubleshooting

tips: I’m just figuring out how to complete my own RTP pipeline, mainly by following ZLMediaKit’s design.
This change avoids unnecessary lock contention in RTP worker threads
and prevents log flooding during track lifecycle transitions.

# Version 0.3.5 - 2026-01- 15
## feat(rtsp/rtp): introduce RTP wire header parsing and packet reorder pipeline

- Add RtpWireHeader to correctly parse RTP wire-format headers (RFC3550)
- Separate RTP wire header from logical RtpHeader to avoid ABI/layout bugs
- Refactor RtpVideoTracker::inputRtp to parse PT/SSRC/SEQ/Timestamp from wire header
- Integrate EnhancedPacketSortor to handle RTP reordering and wrap-around
- Prepare hooks for PT/SSRC locking and NTP-based timestamp conversion
- Lay groundwork for TCP-interleaved and UDP unified RTP input pipeline

tips: I'm just learn how to work jitter buffer

This change fixes incorrect RTP header parsing, prevents random seq/pt/ssrc
misinterpretation, and enables stable real-time packet reordering.

# Version 0.3.6 - 2026-01- 16
## refactor(rtsp): rework RTP over TCP interleaved dispatch and job handling

- Introduce channel-to-track binding for RTP/RTCP interleaved streams
- Pass weak_ptr<RtpTrack> through WorkJob instead of relying on handler-side maps
- Clarify ownership and lifetime between interleaved parser and worker threads
- Prepare worker path for safe RTP packet processing

Note:
Further investigation is required for packet lifetime and use-after-free issues.

# Version 0.3.7 - 2026 - 01 - 19
## fix: resolve RTP packet lifetime issues in worker pool

- Fix heap-use-after-free caused by incorrect ownership of RTP packet memory
- Ensure Packet lifetime is managed consistently across worker threads
- Clarify WorkJob payload semantics (Packet* vs raw buffer)
- Avoid double-free by unifying packet release responsibility
- Improve robustness of RtpJobHandler::handle under concurrent execution

# Version 0.3.8 - 2026-01-20
## rtp: wire RTP parsing into worker → track pipeline
- Add RTP raw packet handling in RtpJobHandler
- Parse and validate RTP headers before track processing
- Construct RtpPacket from raw bytes and hand off to RtpTrack
- Prepare track-level pipeline for ordered RTP processing
- Lay groundwork for jitter buffer and depacketizer integration


# Version 0.3.9 - 2026-01-21
## refactor: add RTCP packet type enums and feedback definitions
- Define RTCP packet type enums (SR/RR/SDES/BYE/APP/RTPFB/PSFB/XR)
- Add SDES item type definitions per RFC3550
- Add RTPFB/PSFB feedback type enums (NACK/PLI/FIR/REMB, etc.)
- Use strongly-typed enum class with protocol-aligned values
- Prepare groundwork for RTCP parsing and statistics handling


# Version 0.4.0 - 2026-01-23
## refactor: rework RTP job payload model and fix abort caused by invalid length'
- Replace void* payload with std::variant
- Use shared_ptr<Packet> for safe cross-thread ownership
- Remove unsafe static_cast paths
- Fix len=0 propagation to inputRtp()

# Version 0.4.1 - 2026-01-27
## feat(rtp): implement RTP parsing, jitter buffering and H264 depacketization

- Improve RTP header parsing and validation
- Integrate jitter buffer for packet reordering
- Implement ordered RTP callback
- Support H264 Single / STAP-A / FU-A depacketization
- Output Annex-B formatted frames

# DEBUG - 2026-01-28
## debug: verify RTP pipeline end-to-end and locate PacketPool exhaustion path

- Verified full RTP flow from worker dispatch to RtpVideoTracker::onRtpSorted
- Confirmed callback and sorting stages are correctly triggered
- Identified potential lifetime and caching issues causing PacketPool exhaustion

# Docs - 2026-01-31
## docs: add RTCP XR (RFC 3611) protocol and SDP signaling notes
- Document RTCP XR common header (PT=207) and packet structure
- Describe the XR report block model and usage scenarios
- Clarify sequence-number–based reporting for Duplicate RLE and Packet Receipt Times blocks
- Explain RTT measurement using Receiver Reference Time (BT=4) and DLRR (BT=5)
- Add SDP a=rtcp-xr signaling rules, distinguishing unilateral and collaborative parameters
- Clarify Offer/Answer behavior and bandwidth considerations for XR usage

This change only updates protocol documentation and design notes, without affecting
existing RTP/RTCP data path logic.

# ROOM:20260204
## feat: introduce ClientSession for per-client send handling

- Isolate per-client RTP send queue and state into ClientSession
- Prepare MediaSession for SFU-style multi-subscriber forwarding

# RoomVersion:0.0.1: 2026-02-05
## feat: add basic SFU room

- Introduce Room module to manage participants and provide basic conference routing.
- Add participant join/leave management and broadcast forwarding logic.
- Each incoming RTP packet is forwarded to all other participants (N-1 fanout).
- Prepare foundation for future subscribe-based routing / simulcast / SVC.

# 2026-02-06
## feat(rtsp/rtp): add RTP packet sorting logs and improve debug tracing
- Add detailed debug logs for RTP sorting pipeline (EnhancedPacketSortor)
- Print seq/next_seq/buffer state to verify jitter-buffer reorder behavior
- Add gdb breakpoint tracing points for emit() / inputRtp() call path
- Improve packet dump helper to validate RTP header correctness
- Facilitate troubleshooting for RTP packet payload/ts/seq parsing issues

# Version 0.4.2 - 2026-02-08
## feat(core): add signal-based subscription framework for stream dispatch
- Add ISubscription / ISignal interfaces for callback subscription model
- Implement SignalCOW with copy-on-write snapshots; emit does not hold the subscription writer mutex
- Provide subscribe/cancel mechanism to manage listener lifecycle
- Introduce SourceBase<T> abstraction to expose publish/subscribe pattern for stream modules
- Prepare foundation for RTP/frame fan-out and modular pipeline extension

# Version 0.4.3 -  2026-02-09
## Add Depacketizer module and start H264 RTP frame reassembly

- Introduced a new Depacketizer module and defined a unified interface (input() / hasFrame() / popFrame()) for frame-level reconstruction based on sorted RTP packets.
- Added initial H264Depacketizer class skeleton, preparing the architecture for future codec extensions (H265/VP8, etc.).
- Completed RTP packet metadata filling after parsing, including ts/marker/version/padding/extension/cc/hdr_len/payload_off/payload_len, ensuring downstream depacketization can rely on correct header/payload boundaries.
- Improved RTP header parsing to correctly handle CSRC and header extensions, with additional validation for padding scenarios.
- Added debug logs for RtpSorted output to verify sequence continuity and payload size variations, confirming correct behavior before implementing FU-A/STAP-A reassembly logic.

# Version 0.4.4 - 2026-02-14
## Introduce RTCP module architecture and prepare RTP/RTCP interleaved processing

- Added initial RTCP module framework following a WebRTC-style interface design, including IRtcpReceiver, IRtcpSender, and IRtcpObserver for clean protocol/business separation.
- Implemented RtcpReceiverImpl skeleton with core entry points (OnRtcpPacket, SetObserver, SetLocalSsrc, SetRemoteSsrc) to prepare for compound RTCP parsing.
- Confirmed RTSP interleaved transport behavior where RTP and RTCP share the same TCP socket, and identified channel-based demux logic (interleaved=RTP-RTCP mapping).
- Prepared media pipeline integration points for routing interleaved RTCP payloads into the RTCP receiver, enabling future support for RR/NACK/PLI feedback handling.
- Reviewed RtspConnection initialization flow and verified worker pool (media) and packet pool integration to support upcoming RTCP parsing and retransmission work.


# Version 0.4.5- 2026-02-15
## Title: Add RTCP receiver integration and fix build/link issues in RTP track
- Introduced RTCP handling interface (inputRtcp) in RtpTrack and implemented RTCP callback mechanism via IRtcpObserver.
- Integrated RtcpReceiverImpl into RtpVideoTracker to support parsing RTCP packets (RR/NACK/PLI events reserved).
- Fixed namespace and constructor signature mismatch for RtcpReceiverImpl (rtcpx::IRtcpObserver*) to resolve undefined reference issues.
- Updated CMake build linkage to ensure RTCP implementation is correctly compiled and linked for unit tests.
Improved RTSP/RTP module structure preparing for future RTCP feedback processing and keyframe request support.

# Version 0.4.6 - 2026-02-16
## Refactor TCP stack by introducing a Session/Observer based architecture.
- Added generic ICodec<Msg> interface to support protocol-level decoding/encoding (SIP/RTMP/RTSP, etc.).
- Implemented ObserverList to broadcast decoded messages to multiple business modules via ISessionObserver.
- Introduced IConnectionObserver and improved connection-to-session callback flow for byte-level events.
- Updated TcpConnection to expose bytes callbacks so TcpSession can take ownership of protocol parsing logic.
- Prepared the framework for multi-protocol session management with clean transport/protocol separation.

tips: Today is Chinese New Year. I stayed in my rented apartment and spent the day coding.


## Commit Message (2026-02-17)
Refactored multi-protocol TCP session architecture by introducing ProtocolDetector and ProtocolDetectorSession, enabling dynamic protocol detection and seamless promotion to protocol-specific sessions (SIP/RTSP), while fixing include dependency and compilation issues in SIP parser integration.

# Version 0.4.7 - 2026-02-18
## Refactored protocol detection and RTSP parser integration by introducing RtspProtocolParser based on ProtocolParser
- Added RtspProtocolParser implementation based on ProtocolParser to support RTSP protocol detection (including $ interleaved framing).
- Refactored parser declarations/definitions and fixed ParseResult scope + missing return issues to resolve compilation errors.
- Updated build integration and linkage to eliminate duplicate Parse declarations and vtable undefined reference errors.

# Version 0.4.8 - 2026-2-20
## Refactor: Introduce protocol factory and session promotion mechanism
- Added ISessionFactory abstraction and integrated factory injection into TcpServer, enabling protocol-level session creation without coupling transport layer to specific protocol implementations.
- Implemented dynamic session promotion in ProtocolDetectorSession: upon successful protocol detection, create concrete session (e.g., RTSP) via factory and replace current session mapping.
- Optimized detection flow to ensure newly promoted session immediately processes existing buffer data, preventing first-packet loss during protocol switch.
- Decoupled RtspSession from RtspServer to reduce strong dependencies and improve modularity of protocol layer.

# Version 0.4.9 - 2026-2-22
## feat(network): introduce UDP server abstraction integrated with EventLoop
- Added UdpSocket encapsulation for non-blocking UDP operations (create/bind/recvfrom/sendto).
- Implemented UdpServer with Channel-based integration into existing Reactor (EventLoop + EpollTaskScheduler).
- Introduced IUdpHandler interface for decoupled datagram processing.
- Enabled TCP and UDP servers to coexist under the same EventLoop.
- Prepared foundation for future RTP/RTCP/ICE integration.

# Version 0.5.0 - 2026-2-23
## feat(media): introduce UDP transport skeleton and session demux layer
- Add MediaEngine as transport entry container to manage UdpServer lifecycle
- Implement UdpMuxHandler to demultiplex STUN / RTP / RTCP / DTLS packets
- Introduce UdpSession abstraction to encapsulate per-peer state
- Define initial session lookup/create mechanism (src-based mapping, preparatory for ICE-lite integration)
- Refactor ownership model: UdpServer uses unique/shared ownership at engine level; sessions hold non-owning reference
- Prepare groundwork for future ICE-lite + RTP integration

# Version 0.5.1 - 2026-2-24
## udp: add peer-based session routing and protocol demux
- Introduce peer -> UdpSession routing in UdpMuxHandler
- Enhance SocketAddr with operator== and custom hash
- Integrate selected peer binding mechanism

wip: unify C++17 standard and fix clangd remote configuration

- Ensure all targets compile with -std=gnu++17
- Fix nested namespace C++17 warning in STUN module
- Install and configure clangd-17 in remote SSH environment
- Explicitly set clangd.path and compile-commands-dir
- Regenerate and link compile_commands.json

Status: build ok, clangd indexing restored

WIP: scaffold ICE/STUN message layer and refactor RTSP parsing

- add initial StunMessage class skeleton (RFC8489 based)
- define STUN header layout (magic cookie, transaction id, attr parsing draft)
- introduce basic AttrType / MsgType enums
- start designing ICE candidate abstraction (placeholder only)

- refactor RTSP request parsing state machine
  * split request line / header / body stages
  * improve CRLF detection logic
  * prepare ANNOUNCE handling entry

- adjust logging granularity for protocol layer
- minor CMake cleanup

NOTE:
ICE connectivity check not implemented yet.
Attribute parsing currently incomplete (no integrity/fingerprint validation).

## 2026-07-26 — RTP 分轨绑定与 Tracker 内存管理

### 本次完成

- 修正 SDP 音视频 Track 与 Payload Type 的绑定逻辑。
- 按具体 media track 查询 PT，避免多路流及音视频重复 PT 相互覆盖。
- 支持绝对、相对 RTSP `a=control` 地址匹配。
- 支持静态音频 PT 0/8（PCMU/PCMA）。
- 检测重复 control，避免后注册 Track 静默覆盖已有 Track。
- 每个 RTP Tracker 使用独立、有界的 Packet Pool。
- 每个 Tracker 默认预分配 64 个 RTP Packet，复用 payload 内存。
- 音频、视频及不同 SSRC 的 Packet Pool 和排序 Buffer 相互隔离。
- 增加内存池耗尽、超大 RTP 包统计。
- 严格限制 RTP 乱序缓存大小，并增加超时清理。
- 补充 SDP Track 绑定、Packet Pool 和排序缓存回归测试。

### 当前 RTP 上行链路

```text
RTSP interleaved 接收
  → channel / Endpoint 分轨
  → SDP Track 与 PT 校验
  → 按 SSRC 创建 Audio/Video Tracker
  → Tracker 独立 Packet Pool
  → RTP 排序 Buffer
  → 音视频 Depacketizer
  → EncodedFrame
```

### 当前弱网与 RTCP 状态

- RTP 有界排序、基本丢包发现和发送端 RTP 重传缓存已经具备。
- RTCP SR/RR、NACK、PLI/FIR、Transport-CC 和 BWE 解析或算法组件已经存在。
- 接收端 NACK 生成、`RtcpDispatcher` 注册、音频 RTCP、PLI/FIR 转发及 BWE 控制动作尚未接入真实媒体链路。
- 下一阶段将优先完成：
  1. RTP 缺包检测 → RTCP NACK → RTP Cache 重传闭环。
  2. SR/RR、RTT、Jitter 与丢包率统计。
  3. PLI/FIR 关键帧恢复。
  4. Transport-CC、BWE 与弱网码率控制。

### 后续待完善

- 修复 RTP 网络入口超过 1500 字节时可能发生的静默截断。
- 减少 `WorkJob → Packet → Tracker Pool` 的中间内存复制。
- 将 `EncodedFrame` 接入录制、播放、转码或其他业务消费 Buffer。
- 接通原始 RTP SFU 订阅转发链路。

## 2026-07-26 — RTP Track Binding and Tracker Memory Management

### Completed

- Fixed SDP audio/video track and Payload Type binding.
- Scoped PT lookup to the corresponding media track, preventing collisions
  between multiple streams or audio/video tracks that reuse the same PT.
- Added support for matching both absolute and relative RTSP `a=control` URLs.
- Added support for static audio PT 0/8 (PCMU/PCMA).
- Added duplicate control detection to prevent a later track from silently
  replacing an existing track.
- Added an independent, bounded Packet Pool to every RTP Tracker.
- Each Tracker preallocates 64 RTP packets by default and reuses payload memory.
- Audio, video, and different SSRCs now have isolated Packet Pools and reorder
  buffers.
- Added counters for Packet Pool exhaustion and oversized RTP packets.
- Enforced the RTP reorder-buffer limit and added timeout-based cleanup.
- Added regression tests for SDP track binding, Packet Pool reuse, and reorder
  buffer limits.

### Current RTP Ingress Pipeline

```text
RTSP interleaved input
  → channel / Endpoint track routing
  → SDP Track and PT validation
  → Audio/Video Tracker creation per SSRC
  → per-Tracker Packet Pool
  → RTP reorder buffer
  → audio/video depacketizer
  → EncodedFrame
```

### Current Weak-Network and RTCP Status

- Bounded RTP reordering, basic packet-loss detection, and the sender-side RTP
  retransmission cache are available.
- Parsing or algorithm components exist for RTCP SR/RR, NACK, PLI/FIR,
  Transport-CC, and BWE.
- Receiver-side NACK generation, `RtcpDispatcher` registration, audio RTCP,
  PLI/FIR forwarding, and BWE control actions are not yet connected to the
  production media pipeline.
- The next phase will prioritize:
  1. Closing the RTP loss detection → RTCP NACK → RTP cache retransmission loop.
  2. Integrating SR/RR, RTT, jitter, and packet-loss statistics.
  3. Adding PLI/FIR-based key-frame recovery.
  4. Connecting Transport-CC, BWE, and weak-network bitrate control.

### Remaining Work

- Fix possible silent truncation of RTP packets larger than 1500 bytes at the
  network ingress boundary.
- Reduce intermediate memory copies across
  `WorkJob → Packet → Tracker Pool`.
- Connect `EncodedFrame` output to recording, playback, transcoding, or another
  application-level consumer buffer.
- Connect the raw RTP SFU subscription and forwarding pipeline.

## 2026-08-08 — 收紧媒体传输接口并统一 RTSP RTP 接收链路

### 本次完成

- 统一 `IMediaTransport` 的发送、接收、连接状态和生命周期接口。
- 新增 `ReceivedMediaPacket`，使用拥有型负载保证接收包能够安全跨线程传递。
- 新增 `IMediaPacketSource` 和 `IMediaPacketSink`，分离网络接收与媒体消费。
- 新增 `MediaTransportBase`，统一管理 Transport ID、原子状态和线程安全的 Sink。
- 实现 `RtspInterleavedTransport`，负责 RTP/RTCP channel 映射及 `$` 帧封装。
- 实现 `UdpMediaTransport`，负责 selected peer 校验和 UDP 数据发送。
- 新增 `MediaEndpointIngress`，统一 endpoint、SSRC、worker affinity 和媒体任务投递。
- 将 RTSP interleaved RTP/RTCP 接收接入 Transport/Ingress 链路。
- 使用 `WorkJob::owner` 管理异步任务引用的接收包内存，移除手工 `new[]/delete[]`。
- 将 RTCP NACK/PLI 发送改为通过 Transport 投递回 TCP IO 线程。
- 在 RTSP 断连和 `TEARDOWN` 时关闭并清理对应 Transport。
- 修复相关 CMake include 依赖和 TCP 接收日志变量错误。

### 当前接收链路

```text
TcpConnection
  → RtspSession（RTSP / '$' 分帧）
  → RtspInterleavedTransport
  → ReceivedMediaPacket
  → MediaEndpointIngress
  → media worker
  → SfuEndpoint / RTP Track
```

### 后续工作

- 由信令层建立 `UdpSession → SfuEndpoint` 的明确绑定后，将 UDP RTP/RTCP 接入同一个 `MediaEndpointIngress`。
- 减少 `MediaEndpoint::OnRtp()` 到 Tracker Packet Pool 之间的剩余内存复制。
- 接通 Room/SFU 的订阅发送链路，使 `RtpSenderTrack` 通过具体 Transport 完成下行转发。

## 2026-08-15 — 接入 RTCP 接收统计与弱网质量评估链路

### 本次完成

- 扩展 `RtpRecvStatsBase`，统一维护每个 SSRC 的 RTP/RTCP 接收统计。
- RTP 到达时实时统计包数、负载字节数、序列号回绕、重复包、乱序包和 RFC 3550 jitter。
- 根据接收时间和累计负载字节数计算平均接收码率。
- SR 到达时保存 NTP/RTP 时间映射、发送包数和发送字节数。
- 根据连续 SR 计算发送端负载码率、发送包速率、SR 间隔、实测 RTP 时钟频率和时钟漂移。
- 实现 `BuildReceiverReport()`，计算 RR report block 所需的 `fraction lost`、`cumulative lost`、`extended highest sequence`、`jitter`、`LSR` 和 `DLSR`。
- 将 `RtpRecvStatsBase` 下沉到 `RtpReceiverTrack`，使音频和视频轨使用同一套统计逻辑。
- 增加 `RtcpDispatcher` 的 SR 完成回调，在 Track 更新完成后通知对应 `SfuEndpoint`。
- Endpoint 根据媒体 SSRC 找到接收轨，将统计结果转换为 `WeakNetFeedback` 并提交给 `WeakNetController`。
- 按 SSRC 独立保存弱网控制器、最近一次 RR 统计结果和网络质量等级，避免音视频状态互相覆盖。
- 视频网络质量进入 `Bad` 状态时，通过现有 RTCP Transport 链路发送 PLI 请求关键帧。
- 网络质量等级发生变化时输出 `[WEAK_NET] quality changed` 日志。
- 补充 SR/RR 接收日志，输出 NTP、RTP timestamp、包数、字节数及 report block 数量。
- 移除 AI Frame 和 Frame Router 的高频逐帧信息日志，保留队列溢出错误日志。
- 补充 RTCP 指标计算和 Dispatcher SR 回调单元测试。
- 补充 `RtpRecvStatsBase` 接口、参数单位、调用时机及状态副作用说明。
- 修复 `OnSenderReport` 调用名称不一致导致的编译错误。

### 当前计算链路

```text
RTP packet
  → RtpReceiverTrack::inputPacket()
  → RtpRecvStatsBase::OnRtpPacket()
  → sequence / loss / jitter / receive bitrate

RTCP SR
  → RtcpReceiverImpl
  → RtcpDispatcher::OnSenderReport()
  → RtpReceiverTrack::OnRtcpSenderReport()
  → TrackClock + RtpRecvStatsBase::OnSenderReport()
  → SfuEndpoint::EvaluateReceiveQuality()
  → BuildReceiverReport()
  → WeakNetFeedback
  → WeakNetController::OnFeedback()
  → NetworkControlUpdate
```

### 后续工作

- 增加独立的质量评估周期，避免完全依赖上游 SR 的发送周期。
- 周期构造并发送 RTCP RR，复用弱网评估已经生成的 report block，避免重复推进统计区间基线。
- 将目标码率、Pacer 和 FEC 建议接入实际媒体发送或编码控制接口。
- 完善下行 `RtpSenderTrack` 的 RR/TWCC、RTT 和带宽估计控制链路。
- 增加 Endpoint/Session 级音视频质量聚合策略。

## 2026-09-14 — 完善录像生命周期并接通发送侧 GCC 反馈链路

### 本次完成

- 拆分 `RecordingSession`、`RecordingInstance` 和 `RecordingSegment`，明确录像会话、执行实例与文件分段的职责。
- 引入 `IRecorder` 和录像事件回调，补充状态流转、停止原因及失败通知。
- 录像先写入临时文件，成功收尾后重命名为正式 MP4 文件。
- 统一编码帧订阅接口为 `SubmitFrame`，同步适配录像和 AI 模块。
- 新增 Transport 级 `SendSideController`，统一管理发送历史与 GCC。
- 音视频及重传共享 TWCC 序号，发送成功后记录包信息。
- 接通 TWCC/RR 反馈处理与目标码率、Pacer 配置输出回调。
- 补充序号回绕匹配、重复反馈过滤和断线状态重置。
- 保留 TWCC 带宽估计，避免 RTT 更新额外改变目标码率。
- 拆出 `media_quality` 库，明确发送轨与控制器的链接依赖。
- 新增 8 个发送侧链路测试及[接入文档](media/quality/README.md)。

### 验证情况

- 发送侧与原有弱网测试共 16 项，独立构建运行通过。
- 完整服务及录像改动尚未完成本轮联调。

### 后续工作

- 当前 GCC 链路接至控制输出接口，继续接入实际 Pacer 发包和编码器调码率。
- 实现 TWCC 超时后的 RR 回退策略。

## 2026-09-23 — 重构 SFU 端点，支持多轨发布与订阅

### 本次完成

- 将 SDP 媒体描述统一转换为现有 RTP Track 参数，复用 `StreamContext` 和 `SdpTrackBinding`。
- 支持单个 `SfuEndpoint` 管理多条发布轨道，按 MID、SSRC 和轨道提示分流。
- 将接收 Track 归属发布端点、发送 Track 归属订阅端点，通过工作线程投递完成跨端点转发。
- 接通房间发布、订阅、取消订阅、取消发布和参与者退出的媒体生命周期。
- 支持下游 RTP 参数及 MID/TWCC 改写，接通 NACK 缓存重传和 PLI 反馈。
- 调整 RTSP 多轨共享端点，完善 SETUP 失败回滚和 TEARDOWN 清理。
- 新增 `WebRtcMediaTransport`，适配 WebRTC 会话与统一媒体收发接口。
- 完善订阅失效、排队旧包丢弃及端点停止后的资源清理。
- 补充多轨转发、房间订阅、SDP 绑定、WebRTC 传输适配和 RTSP 多轨集成测试。

### 验证情况

- 主程序编译通过，新增 10 项测试全部通过，RTSP、SDP 和录制相关回归通过。
- 完整回归仍有两项已知失败：WebRTC STUN 提名、超大 RTP 包计数。

### 当前限制

- WebRTC 应用层信令组装及实际 DTLS/SRTP 后端仍待接入，尚未完成真实浏览器链路验证。
- 暂不支持 RTX 和 simulcast 层选择；每条订阅固定转发一个源编码的 SSRC。
- 房间接入仍需应用层绑定参与者端点，并根据下游协商结果配置订阅参数。

## 2026-09-24 — 引入公共状态机并接入 ICE 与 RTSP 会话

### 本次完成

- 新增 `StateMachine` 和 `StateController`，支持强类型转换表、条件判断、动作回调和绑定业务上下文。
- 明确非法事件、重复规则、递归派发和异常处理语义。
- 将 ICE 生命周期改为显式状态，接通存活超时与 WebRTC 会话关闭，阻止关闭后的连接被迟到包激活。
- 修复 STUN MESSAGE-INTEGRITY 计算范围错误，防止认证失败的请求污染远端凭据及未认证属性触发提名。
- 将 RTSP 推流流程接入状态机，保留多轨 SETUP 和失败回滚，成功处理请求后才提交状态转换。
- 校验 RECORD/TEARDOWN 的会话归属，支持重复 RECORD、TEARDOWN 后复用 TCP 连接及断线清理。
- 未实现的 DESCRIBE、PLAY、PAUSE 明确返回 501。
- 补充公共状态机、ICE 和 RTSP 生命周期测试；新增 [ICE 接入说明](protocol/ice/README.md)及 [RTSP 状态机说明](Rtsp/StateMachine.md)。

### 验证情况

- 主程序编译通过，公共状态机、ICE、WebRTC 会话、传输及媒体适配相关测试通过。
- RTSP TCP/UDP、多轨 SETUP、失败回滚、错误请求顺序、会话重建和断线清理测试通过。
- 上一条记录中的 STUN 提名失败已修复；超大 RTP 包计数问题不在本次修改范围内。

### 当前限制

- 应用需在会话事件循环周期调用 `WebRtcSession::Tick`，确保无流量时也能及时检测 ICE 超时。
- ICE 建立与存活超时仍共用配置；会话级 ICE restart、完整主动检查及真实浏览器互通仍待完善。
- RTMP、AI 和公共服务生命周期尚未迁移至状态机组件。
