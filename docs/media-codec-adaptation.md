# 多编码适配实现设计

状态：已实现 H264、H265、VP8、VP9、AV1 的软件编解码适配、工厂选择和多档混合输出。覆盖的是完整编码帧进入计算层、转码后输出完整编码帧；各格式的 RTP、协商、容器及播放器互通仍按业务链路分别交付。

业务边界与协议支持矩阵见 [全业务媒体格式兼容方案](media-format-plan.md)。本文先记录实际实现，随后保留音频、硬件、动态控制及全业务接入的后续设计。目标仓库为 Packetia-sfu，参考 Watermelon 的职责划分，采用本项目的 C++17、RAII 和任务 worker。

## 已实现的计算层

```text
TranscodeSession：任务 worker、有界队列、关键帧恢复、输出路由
    ↓
VideoTranscoder：输入时间戳归一化、一次解码、多档编排、轨道身份、统计
    ↓ VideoCodecFactory：格式 / 方向 / 参数 / 实现偏好
IVideoDecoder → 拥有存储的 RawVideoFrame → 每档 scaler → 每档 IVideoEncoder
    ↓ 完整编码帧，可由后续 RTP / 容器 / 本地消费者接入
```

### 实现与本机验证

| 格式 | 解码实现（按 Auto 优先级） | 编码实现（按 Auto 优先级） | 当前输出约束与验证 |
| --- | --- | --- | --- |
| H264 | `h264` | `libx264` | Baseline/Main/High；Annex B、无 B 帧、重复 SPS/PPS；三种 profile 均核对真实码流 |
| H265 | `hevc` | `libx265` | Main、8-bit I420；Annex B、关闭 open GOP/B 帧、关键帧携带 VPS/SPS/PPS；已验证 |
| VP8 | `vp8`、`libvpx` | `libvpx` | VP8 完整编码帧、8-bit I420、关闭 lookahead/alt-ref；编码及两种解码均已验证 |
| VP9 | `vp9`、`libvpx-vp9` | `libvpx-vp9` | profile 0、8-bit I420、完整 VP9 编码帧；编码及两种解码均已验证 |
| AV1 | `libdav1d`、`libaom-av1` | `libsvtav1`、`libaom-av1` | profile 0、8-bit I420、OBU；本机验证 SVT-AV1 编码和 dav1d 解码；本机 FFmpeg 未提供 libaom，相关路径未实测 |

以上都是经 FFmpeg API 包装的实现；没有直接调用 x264/x265/libvpx/SVT/dav1d 的独立库适配，也没有启用硬件后端。节点所用 FFmpeg 构建不包含某实现时，该候选的 `library_available` 为 false。

SVT-AV1 低延迟模式明确配置 CBR、单个 logical processor、关闭 lookahead/temporal filtering；只设置 FFmpeg `bit_rate` 会选择不受该低延迟模式支持的 VBR，不能据此宣称初始化成功。x265 关闭内部线程池并使用一个 frame thread，其余适配器也设置单线程；这不是容量或时延承诺。

每档可独立选择 codec、尺寸、bps 和实现。输出编码输入统一为有限范围、方形像素的 I420 8-bit，缩放器保持显示宽高比并补黑边。当前验收为 SDR；不提供 HDR 色调映射，也不宣称完整支持 10-bit/SVC/多层编码。

### 参数、工厂及码流表示

| 实际代码 | 职责 |
| --- | --- |
| [VideoCodecConfig.h](../media/codec/VideoCodecConfig.h) | 不依赖 FFmpeg/帧缓冲区的纯配置：实现选择、参数 variant、编码/解码配置、错误与能力结果 |
| [VideoCodecTypes.h](../media/codec/VideoCodecTypes.h) | CPU 原始帧平面、stride、色彩、SAR、时间戳与共享存储 |
| [IVideoCodec.h](../media/codec/IVideoCodec.h) | 解码、缩放和编码的串行同步接口及生命周期契约 |
| [VideoCodecFactory.cpp](../media/codec/VideoCodecFactory.cpp) | 软件实现白名单、方向/codec ID 校验、参数过滤、能力发现及初始化回退 |
| [FfmpegVideoCodec.cpp](../media/codec/FfmpegVideoCodec.cpp) | 实现相关参数转换、库缓冲区、各格式输入与输出元信息 |
| [TranscodeTypes.h](../service/TranscodeService/TranscodeTypes.h) | 任务指定输入 codec/decoder，每个 rendition 指定输出 codec/backend/parameters |
| [VideoTranscoder.cpp](../service/TranscodeService/VideoTranscoder.cpp) | 组合接口，不直接调用编码、解码或缩放 API |

`VideoCodecParameters` 是带类型的 variant：默认 monostate 使用所选 codec 的默认值；专有参数类型与 codec 不一致时拒绝。H264 支持 Baseline/Main/High；VP9/AV1 的非 0 profile 明确返回 Unsupported。尺寸、码率、帧率是公共边界过滤，具体实现仍可施加更严格限制，例如 SVT 的最小尺寸。

`EncodedFrame::bitstream` 显式区分 Annex B、VP8 frame、VP9 frame、AV1 OBU；H264/H265 以外的字节不经过 H264 NAL 扫描。H265 的 VPS/SPS/PPS、IDR 按 HEVC NAL 类型识别。通用关键帧依赖 `frame_type`，不把 VP8/VP9/AV1 的 key frame 标成 H264 IDR。

解码器拒绝 codec 不匹配、不完整帧、错误码流表示和缺少时间戳的输入。为兼容已有 H264 生产者，仅 H264 允许 `bitstream=Unspecified` 并按 Annex B 校验；新格式输入必须提供正确的 bitstream 字段。H26x 配置帧及 AV1 sequence-header OBU 可以在直接 decoder 接口暂存后与首个图像拼接，暂存上限 1 MiB；VP8/VP9 没有独立参数集帧。任务首次接入/恢复要求包含必要配置的随机访问关键帧。

实现选择规则：

- Auto 忽略实现名，按注册顺序尝试同格式候选。
- Prefer 先尝试请求名称，再尝试同格式其他候选；名称未注册或库缺失也可回退。
- Require 必须指定已注册、方向与 codec 匹配的名称，失败即返回；不跨格式回退。
- 实际初始化失败后销毁部分创建的资源，再尝试下一候选。全部失败的错误包含各候选名称和原因。运行中不自动切换实现或 codec。

`Capabilities()` 分别报告适配器已编入、库实现能否发现；它不预留资源，也不保证本次 profile/尺寸能成功初始化。`CreateEncoder/CreateDecoder` 返回已完成 Open 的实例。即使计算节点可编码某格式，也不表示 WebRTC SDP、RTP、录像容器或目标播放器已接受该格式。

### 任务配置示例

下面要求上游已交付完整 H265 Annex B 编码帧，生成 H264 和 VP9 两路。它不代表当前 RTSP/RTP 入口已能接收 H265。

```cpp
service::transcode::VideoTranscodeConfig video;
video.input_codec = media::CodecType::H265;
video.decoder = {media::codec::SelectionMode::Require, "hevc"};
video.renditions = {
    {"720p-h264", 1, {1280, 720, 2000000}},
    {"360p-vp9", 2, {640, 360, 600000}},
};
video.renditions[0].codec = media::CodecType::H264;
video.renditions[0].backend = {media::codec::SelectionMode::Require, "libx264"};
video.renditions[0].parameters = media::codec::H264Parameters{media::codec::H264Profile::High};
video.renditions[1].codec = media::CodecType::VP9;
video.renditions[1].backend = {media::codec::SelectionMode::Prefer, "libvpx-vp9"};
video.renditions[1].parameters = media::codec::Vp9Parameters{0};

service::transcode::TranscodeTaskConfig task;
task.task_id = "camera-multi-codec";
task.input = {10, 42};                       // endpoint + track，按实际来源填写
task.output = {20, "session", "camera"};     // 与输入不同的输出身份
task.video = video;                         // MediaTime；原始 RTP32 输入须显式修改
// app.StartTranscode(task)，或调用 TranscodeService::StartTask(task)
```

默认配置仍为 H264 输入和 720p/360p 两路 H264 输出，原有调用方可继续使用。

### 线程、所有权和未完成部分

所有工厂/编解码/Flush/Close 操作仍在任务 worker 串行执行；路由同步回调只入有界队列。共享原始帧存储保留所有平面，FFmpeg decode/scaler 复用缓冲区时不会覆盖已返回帧。同后端编码保留存储引用，外部 CPU 存储复制成 FFmpeg 自有帧。调用方应保证平面容量，并在使用期间保持像素只读。

队列恢复按通用关键帧准入；状态字段 `waiting_for_idr` 保留历史名称，五种格式都表示等待可随机访问的关键帧。输入 SSRC 改变、切换 codec 或 profile/设备需要显式重建任务，不能把新的码流字节直接喂给旧实例。

`packetia_transcode_domain` 只依赖纯配置与身份类型，不链接 FFmpeg、帧缓冲区、线程或路由器；计算层目标 `packetia_video_codecs` 独立于房间和服务。缺少软件库时，由能力查询/初始化给出具体结果。

后续范围：其他格式的 RTP 组帧/封包及会话绑定、多格式容器输出、独立库/硬件后端、音频转码、运行时码率/关键帧控制、网络反馈和业务处理计划。下文涉及这些能力的完整接口仍是设计草案，真实接口以头文件为准。

接收侧已补齐通用 RTP 排序队列的回绕、容量和等待期限，以及 H264 帧缓冲超时与媒体 worker 空闲检查，见 [RTP 排序队列与等待期限](rtp-reorder-queue.md)。这部分限制收包与组帧等待，不等同于转码任务队列或端到端延迟控制。

## 1. 参考依据与目标项目的新设计

本机参考根目录为 `/Users/kxy/code/rcs/rcs/sdk/watermelon`，下表路径均相对其 `webrtc/trunk/src`。核对的是带 `jssmme`/ZMF 扩展的早期 WebRTC 定制分支，不代表现代上游 WebRTC 接口或当前目标平台的能力。

| 已核对的参考源码 | 可借鉴的机制 | Packetia 的设计选择 |
| --- | --- | --- |
| `common_types.h`：`VideoCodec`、`VideoCodecUnion` | 格式、通用参数、专有参数、`plType`、`implName` 分工 | C++17 值类型和 `std::variant`；PT 留在会话绑定中，具体实现偏好留在后端请求中 |
| `modules/video_coding/codecs/interface/video_codec_interface.h`：`VideoEncoder`、`VideoDecoder` | 初始化、提交帧、输出、关键帧、码率、网络信息、释放 | 延续当前任务 worker 上串行执行的同步公共接口，底层库回调由适配器收敛 |
| `modules/video_coding/main/source/codec_database.cc`：`CreateEncoder/CreateDecoder` | 按格式、编译条件和实现能力选实例 | 明确区分 Auto、Prefer、Require；记录候选失败原因，以 RAII 清理部分初始化 |
| 同文件：`RegisterReceiveCodec/SetDecoder/CreateAndInitDecoder` | 注册接收配置，在 PT 变化后选择/初始化解码器 | 使用传输会话、MID、方向和配置代次区分绑定；复用目标仓库的 BUNDLE 路由规则 |
| `modules/rtp_rtcp/source/rtp_sender_video.cc`、`rtp_receiver_video.cc` | 发送按格式分发到 payload 实现，接收注册 PT 对应格式 | 各编码使用自己的 RTP packetizer/depacketizer，统一复用传输、调度和反馈 |
| `video_engine/vie_encoder.cc`：`OnNetworkChanged`；`modules/video_coding/main/source/generic_encoder.cc`：`SetRates` | 网络目标经统一接口送入具体编码器 | 对接现有 `SendSideController`；公共单位统一 bps、Hz、ms，库单位在适配器转换 |
| `modules/audio_coding/main/source/acm_codec_database.cc`：`CreateCodecInstance` | 音频按编码、采样率、声道创建对应实现 | 音频独立接口和重采样链路，不复用视频的尺寸和 IDR 规则 |

参考实现有值得保留的限制：启用 JSS 时可能先于内置格式分支选择外部实现，`implName` 并非所有路径上的强制选择器；H265 枚举不保证独立软件编码器存在；AV1 编码与解码走不同实现和构建条件。Packetia 的选择语义需要独立定义。

参考接收库允许注册多种格式，不表示单个实例同时解码多种格式。`SetDecoder` 在切换已注册 PT 时通常释放旧实例并初始化新实例；目标项目按每条轨道的配置代次管理解码状态，不照搬历史特殊值和原始指针所有权。

## 2. 层次与调用链

```text
观看 / 录像 / 推流 / 合成 / 分析
    ↓ 输入 TrackFormat + 目标 OutputContract
MediaProcessingPlan：透传、换封装、仅音频转码、音视频转码
    ↓ 资源准入、实际能力过滤
TranscodeService → TranscodeTask → TranscodeSession
    ↓ 任务 worker 串行执行
解码适配器 → 原始帧转换 → 多个编码适配器
    ↓ 自有编码帧、实际输出格式与配置代次
目标适配器：RTP 封包 / 容器封装 / 本地帧消费者
```

RTP 接收路径：网络 owner → 解密与会话路由 → 媒体 worker → 对应格式解包/组帧 → 编码帧 → 任务队列 → 解码器。

RTP 发送路径：编码器 → 对应格式封包器 → 已有 RTP sender 的序号、缓存与反馈 → 传输 owner → 加密与网络发送。当前 `RtpSenderTrack` 已有 RTP 转发、重写及重传相关能力，编码帧到 RTP 的 packetizer 是需要补齐的另一层。

纯 SFU 转发不强制经过解码器；录像若只需改变码流表示或容器，也不应创建视频编码器。能力判定分别检查实际经过的环节。

## 3. 参数与数据模型

四类信息分别管理：

| 信息 | 内容 | 不承担的职责 |
| --- | --- | --- |
| 编码格式 | H264/H265/VP8/VP9/AV1、音频编码和专有参数 | 不选择 x264、硬件设备或 PT |
| 实现选择 | 后端、实现名、软硬件类型、设备、选择模式 | 不直接更改协商格式 |
| 原始帧格式 | I420/NV12、位深、色彩、PCM 布局和采样 | 不假设任意后端支持任意内存/格式 |
| 传输/封装绑定 | 会话、MID、PT、fmtp、RTP 时钟、码流表示、容器 | 不作为全局 codec 编号 |

下列代码表达拟议的参数形状，使用独立命名空间区分草案；尚不构成已注册的编码实现。

```cpp
namespace planned {
enum class VideoCodec { H264, H265, VP8, VP9, AV1 };
enum class AudioCodec { AAC, Opus, PCMA, PCMU };
enum class Direction { Encode, Decode };
enum class SelectionMode { Auto, Prefer, Require };
enum class PixelFormat { I420, NV12 }; // 首轮的原始视频格式候选
enum class PcmFormat { S16Interleaved, F32Planar };

struct H264Params { std::string profile_level_id; };
struct H265Params { int profile_id = 1; int level_id = 0; };
struct Vp8Params {};
struct Vp9Params { int profile_id = 0; };
struct Av1Params { int profile = 0; int level_index = 0; int tier = 0; };
using VideoSpecific = std::variant<H264Params, H265Params, Vp8Params,
                                   Vp9Params, Av1Params>;

struct VideoFormat {
    VideoCodec codec = VideoCodec::H264;
    VideoSpecific specific = H264Params{};
    int bit_depth = 8;
};
struct VideoEncodeConfig {
    VideoFormat format;
    int width = 0, height = 0;
    int fps_num = 30, fps_den = 1;
    uint64_t bitrate_bps = 0;
    uint32_t keyframe_interval_frames = 60;
    bool low_latency = true;
};
struct AudioFormat {
    AudioCodec codec = AudioCodec::Opus;
    int sample_rate_hz = 48000;
    int channels = 2;
    std::string channel_layout;
};
struct BackendRequest {
    SelectionMode mode = SelectionMode::Auto;
    std::string backend;        // 例如 ffmpeg，负责适配 API
    std::string implementation; // 例如 libx264，指实际选用实现
    std::string device;         // 后端定义的设备标识，软件实现可为空
};
} // namespace planned
```

完整实现还需要 AAC object type、Opus 帧时长/FEC/DTX 等音频专有参数，并校验 codec 与 variant 分支一致。H264 packetization-mode 属于 RTP 协商约束，编码适配器可据此配置输出，但不把它当编码库身份。

原始帧应使用带所有权的存储：CPU 帧含平面视图、stride、尺寸及色彩信息；硬件帧含带释放器的设备句柄、设备/内存域和同步约束。一个裸指针或 `PixelFormat::NV12` 不能说明能否零拷贝交给另一设备。

复用并扩展现有 `media::EncodedFrame`：保留 PTS、DTS、duration、time base、随机访问语义、码流表示、初始化数据及配置代次。编解码层不要求编码结果携带 RTP PT/SSRC，RTP 专有信息由输出绑定生成。

## 4. 能力发现与平台支持

能力查询至少包含：方向、编码及专有参数、原始格式、尺寸/帧率/码率或采样/声道范围、设备、延迟模式，以及动态更新能力。查询结果分为：

1. **构建包含**：适配器和依赖是否编入或可动态加载。
2. **运行可用**：库、设备、驱动、会话配额是否可访问。
3. **配置支持**：该实例是否接受此次 profile、尺寸、位深、内存格式等配置。
4. **目标兼容**：传输、容器和接收端是否接受实际输出。

| 候选路径 | 如何判定 | 当前情况 |
| --- | --- | --- |
| FFmpeg 软件解码 | 查询对应 `AVCodecID` 的解码器，再实际初始化 | 已接入五种视频编码的指定软件 decoder；实际可用候选见文首 |
| FFmpeg 软件编码 | 查询明确实现名及能力，再实际初始化 | 已接入 x264/x265/libvpx/SVT-AV1，libaom 候选本机未实测 |
| OpenH264/libvpx/libaom 等独立适配 | 对应构建目标、真实库 API、运行和配置查询 | 未新增；FFmpeg 包含某库也不等于独立适配已实现 |
| macOS 硬件后端 | VideoToolbox 等的设备、格式和会话能力 | 规划候选，未验证 |
| Linux 硬件后端 | VAAPI/NVENC 等的构建、设备和驱动能力 | 规划候选，未验证 |

CPU 与硬件不组成对外编码格式列表。比如一个节点能编码 H264，但可能只支持某些分辨率或无法再创建硬件会话。静态查询只是候选过滤，实例初始化仍可能因资源变化失败。

继续沿用系统 FFmpeg 构建方式。新增音频重采样时声明 `libswresample` 依赖；硬件/独立库按独立 CMake 选项或目标接入，不无条件引入全部 SDK。对外支持列表从已启用模块和实际能力生成，不能直接遍历 codec 枚举。

## 5. 计算接口、线程和所有权

保留现有 `IVideoTranscoder` 作为任务内的编排入口，逐步将具体解码器、编码器和原始帧转换收进计算适配层。控制语义先定义，再按实际实现增加能力，避免直接给不支持的后端返回假成功。

下面是接口草案。`CodecStatus`、配置及 raw frame 类型需在实施时定义；以引用或共享只读帧传递媒体，禁止由调用方在输入后复用仍被后端持有的内存。

```cpp
class IVideoEncoder {
public:
    virtual ~IVideoEncoder() = default;
    virtual CodecStatus Open(const VideoEncodeConfig&) = 0;
    virtual CodecStatus Encode(std::shared_ptr<const RawVideoFrame>,
                               std::vector<media::EncodedFrame::ConstPtr>&) = 0;
    virtual CodecStatus RequestKeyFrame() = 0;
    virtual CodecStatus UpdateRates(const VideoRateUpdate&) = 0;
    virtual CodecStatus SetNetworkState(const EncoderNetworkState&) = 0;
    virtual CodecStatus Flush(std::vector<media::EncodedFrame::ConstPtr>&) = 0;
    virtual void Close() noexcept = 0;
};

class IVideoDecoder {
public:
    virtual ~IVideoDecoder() = default;
    virtual CodecStatus Open(const VideoDecodeConfig&) = 0;
    virtual CodecStatus Decode(media::EncodedFrame::ConstPtr,
                               std::vector<std::shared_ptr<const RawVideoFrame>>&) = 0;
    virtual CodecStatus Flush(std::vector<std::shared_ptr<const RawVideoFrame>>&) = 0;
    virtual void Close() noexcept = 0;
};
```

音频对应独立的 `IAudioEncoder`、`IAudioDecoder`，输入输出使用 `RawAudioFrame`/编码音频帧、采样率和声道配置；它们不提供视频关键帧请求。重采样器负责采样格式、采样率和布局转换，延迟和剩余样本参与 Flush。

统一结果至少区分 `Unsupported`、`InvalidConfig`、`Unavailable`、`ResourceExhausted`、`InitializationFailed`、`RuntimeFailed`、`ReconfigureRequired`。错误还应含方向、codec、请求/实际实现、阶段和候选失败原因。编码成功但暂时没有输出是缓冲行为，不等于失败。

线程与资源契约：

| 调用点 | 线程/所有权约定 |
| --- | --- |
| 帧路由和媒体接收 | 同步回调，快速入有界队列；不执行耗时初始化或编码 |
| 工厂、Open、Encode/Decode、控制更新、Flush、Close | 在任务所属 worker 串行执行；控制请求与媒体帧统一排序 |
| 输入帧 | 提交后保持只读；适配器若延迟使用，持有拥有存储的引用 |
| 返回帧 | 拥有有效存储；不能指向下次 receive/unref 后失效的库缓冲区 |
| 第三方内部异步回调 | 由适配器保留状态和代次，转换为公共同步结果；不直接回调业务对象 |
| Flush | 停止接受新输入后输出延迟结果；失败按任务策略报告 |
| Close/析构 | 可重复释放，等待或取消内部在途操作，返回后不再发生访问该实例的回调 |

同步公共接口适配硬件时需要有界等待和超时策略。不能让一个永不完成的硬件回调阻止整个任务关闭；也不能在媒体接收线程或输出回调里等待自身 worker 退出。

## 6. 工厂与回退规则

工厂按 codec、方向、配置和实现偏好选候选。初期使用集中式表/分支即可，候选数量增长后再引入注册表。

| 模式 | 行为 |
| --- | --- |
| Auto | 从满足同一格式及配置要求的可用实现中按策略选择 |
| Prefer | 优先指定实现；不可用或初始化失败时允许尝试其他同格式实现 |
| Require | 只允许指定实现，失败即返回错误，不静默替换 |

选择流程：校验请求 → 构建/运行能力过滤 → 排序 → 实际创建和初始化 → 失败候选完全释放 → 尝试下一个 → 返回实例及实际选择记录，或聚合错误。

同格式回退也要重新比较 profile、位深、原始格式和内存域；例如硬件 NV12 失败后换软件 I420，必须包含下载/转换并满足延迟、资源预算。不能只替换实现名字。

跨格式回退由 `MediaProcessingPlan` 重新决策，不放进编码器工厂。例如 AV1 → H264 必须同时具备业务授权、目标端兼容能力、已协商的 RTP 映射或新的协商，以及对应封包器/容器。保持 PT 不变而更换其编码含义不可接受。

运行中失败也不能把旧参考帧直接交给新实现继续编码。需要新配置代次、随机访问输出和目标更新，再恢复交付；不能把初始化回退流程直接套用到正在发送的实例。

## 7. RTP 与会话映射

沿用 `WebRtcSession` 当前按 MID 和 PT 保存绑定的思路：动态 PT 的解释只在具体传输会话和媒体方向内有效。发生 BUNDLE 歧义时先根据 MID/SSRC 找到媒体段，再查绑定，不能全进程用 `PT=96 → H264`。

建议绑定内容：`transport/session + MID + direction + generation → PT、RTP clock、codec、协商参数、头扩展、反馈、解包/封包实现`。参考分支的单层 PT 查表需要按目标项目的 BUNDLE 规则扩展。

接收时：分类解密 → MID/SSRC/PT 校验 → 按绑定选 payload 解析 → 组完整帧并携带配置代次 → 按格式选择解码器。未知 PT 或不兼容参数丢弃并记录可观察错误，不猜测编码，也不让未认证包触发任意实例创建。

发送时：实际编码结果 → 校验输出仍匹配协商 → 按格式封包 → 分配输出 SSRC/序号/时间戳 → RTP sender 缓存和扩展 → 加密发送。透传路径按自身重写策略处理，不能误用输入包的传输身份作为转码输出身份。

| 格式 | 独立 payload 适配的重点 |
| --- | --- |
| H264 | packetization-mode、单 NAL/聚合/分片、SPS/PPS、marker 和帧完整性 |
| H265 | VPS/SPS/PPS、聚合/分片头、协商相关字段、随机访问点恢复 |
| VP8 | payload descriptor、分区边界、PictureID 等可选字段 |
| VP9 | payload descriptor、PictureID、帧/层依赖和帧边界 |
| AV1 | 聚合头、OBU 分片和长度表达、sequence header 与新序列状态 |
| 音频 | 采样时钟、包时长；AAC AU header 与配置；Opus/G711 各自 payload 约束 |

实施时按对应 payload 规范和已知兼容样本核对线格式，不能因为都采用 RTP 就共用 H264 分片头。MTU 预算要扣除实际 IP/UDP/RTP、扩展、SRTP 及采用中继时的相关开销；重传路径也要遵守预算。

## 8. 动态控制、切换与配置代次

| 变化 | 默认处理策略 | 例外与确认点 |
| --- | --- | --- |
| 目标码率、帧率 | 后端声明支持时调用更新接口 | 不支持返回 ReconfigureRequired，由业务决定重建；报告实际应用值 |
| 关键帧请求 | 投递到产生该输出的编码器 | 按编码器能力实现；成功接纳请求与实际产出分开统计 |
| 输入原始格式、分辨率、声道 | 转换或重建对应计算资源 | 保留时间线；消费者不能继续使用过期格式描述 |
| codec、profile、位深或设备 | 默认建立新实例/配置代次 | 输出协商、内存转换、配置数据和恢复点同时更新 |
| RTP 映射变化 | 按 offer/answer 或协议约定提交 | 当前 WebRTC 会话对已有 MID 内 PT 语义有稳定性校验，不能绕过该规则强行切换 |

切换顺序：准备候选和新绑定 → 暂停旧代次入队 → 按策略排空或取消旧输出 → 满足协议协商条件后提交新代次 → 发送配置和随机访问输出 → 恢复交付 → 回收旧实例。失败时仅在旧实例及其绑定仍有效的情况下回退，否则明确终止输出。

所有输入、控制请求和输出都带代次，迟到回调或队列旧帧不能冒充新格式。是否保留 SSRC、包序号连续性、清空接收缓冲和重建订阅由实际协议决定，不使用统一的“切换必换 SSRC”规则。

同一接收轨道同时注册多种格式后，活动实例仍按当前帧对应的已提交绑定选择；多条并行编码流则分别维护实例和时间线。

## 9. 网络控制接入

```text
TWCC / RR / 丢包、RTT
    ↓
现有 SendSideController → NetworkControlUpdate
    ↓
媒体策略：每传输预算、档位选择、必要的目标码率/帧率分配
    ↓ 投递到对应任务 worker
UpdateRates / SetNetworkState / RequestKeyFrame
    ↓
具体编码适配器转换参数并执行，回报实际结果
```

Packetia 当前已有网络控制输出接口，当前转码接口尚无上述动态控制方法，二者还没有自动贯通。BWE 不是 codec 工厂，新增编码复用带宽估计流程，格式相关行为由策略和适配层负责。

公共接口使用码率 bps、帧率有理数/明确的 fps、采样率 Hz、RTT ms、丢包率 0..1；具体库需要 kbps 或 0..255 丢包值时由适配器做有范围检查的转换。参考源码 `OnNetworkChanged` 中存在 bps 到 kbps 的转换，不能按旧字段名称直接传值。

转码输出被多人共享时，不能让每个观众的反馈直接覆盖共享编码器码率。先由策略决定订阅档位、允许的预算聚合或创建独立输出。Pacer 的 pacing bitrate 也不能直接当成媒体编码码率，音频、重传和协议开销需要分别预算。

网络更新在 worker 上合并应用，避免请求积压；后端失败时保留已确认配置、报告错误并按策略重建/降级。文件转码、固定规格归档可不使用实时 BWE，反馈不应改变其交付规格。

## 10. 对现有代码的实施落点

以下为完整实施清单，当前落地情况见文首；其余按技能要求的依赖顺序推进：

| 步骤 | 现有落点/拟议模块 | 需要完成的改动 |
| --- | --- | --- |
| 参数与能力 | `media/MediaFrame/MediaFrameTypes.h`、轨道描述、拟议格式与能力模块 | 编码身份映射、专有参数、原始格式、位深、码流表示、方向与配置能力 |
| 实现适配 | `service/TranscodeService/VideoTranscoder.cpp`、`media/codec` | 已接入五种视频格式、独立 scaler 和实现参数；后续补音频、硬件和动态控制 |
| 工厂注册 | 当前 `VideoTranscoderFactory` 与 `VideoCodecFactory` | 前者替换整条转码计算，后者已按格式/方向过滤并在同格式候选间执行初始化回退 |
| 传输适配 | `Common/Depacketizer`、`Rtsp/RtpReceiver.cpp`、拟议 packetizer | 补各格式组帧/封包和线格式验证，明确纯转发与帧处理的准入 |
| 会话映射 | `server/WebRtcService.cpp`、`protocol/WebRtc/WebRtcSession.cpp`、SDP | 消除 H264/Opus 固定订阅映射，只开放实际可用且已协商的完整路径 |
| 生命周期与动态控制 | `TranscodeSession`、`SendSideController`、输出业务适配 | worker 内实例管理、控制队列、代次、关键帧、网络目标应用和失败回收 |
| 容器与业务交付 | 录像、RTMP/RTSP、HLS/文件等目标适配 | 每个容器验证参数与码流表示；输出发布、协议及任务接口分别验收 |
| 构建与平台 | `cmake/ffmpeg`、转码模块 CMake、各独立后端目标 | 明确新增库和平台条件；缺少某后端时报告能力缺失而非假成功 |

不要仅在 `CodecType` 添加枚举、把 FFmpeg decoder ID 换成变量，就宣布新增格式完成。完整交付还依赖输入、输出、参数、控制和真实对端验证。

## 11. 验证与能力声明

| 验证层 | 关键用例 | 能证明什么 |
| --- | --- | --- |
| 能力与选择 | 方向、编译缺失、设备不可用、配置不支持；Auto/Prefer/Require；初始化失败和清理 | 选择策略和资源回收正确，不能单独证明编码可用 |
| 真实计算 | 每种可用实现编码后独立解码；核对参数、时间戳、随机访问、排空和释放 | 此环境与配置的编解码路径可用；有损编码使用合理画质/误差指标 |
| 音频 | 重采样、布局、priming/delay、尾部样本、长时间音画同步 | 音频转换及与视频共同时间线正确 |
| RTP | MTU、分片、乱序、丢包恢复、回绕；两个会话相同 PT 不同 codec；BUNDLE 相同 PT 由 MID 区分 | 对应传输与会话映射正确；必须含已知兼容对端或协议样本 |
| 动态行为 | 码率/帧率更新、关键帧、重建、codec 切换、旧代次迟到帧、停止竞态 | 更新后输出仍能被正确接收和解码 |
| 业务交付 | 浏览器观看、音视频录像、指定协议播放、单输出失败和任务结束 | 被验收的整条业务组合可用 |

硬件真实测试缺设备时标为“未验证”，不使用 mock 冒充硬件支持。对称自收自发不能代替线格式互通，库能初始化也不能代替浏览器可播放。

当前真实计算验证包括五种编码的独立 FFmpeg 解码、随机访问关键帧从中途启动、PTS/尺寸/图像内容、Flush 和存储生命周期；五种输入分别同时生成五种输出，共 25 种转换组合。服务测试覆盖五种格式按配置路由、关键帧准入和正常停止排空。

新增测试还核对 H264 三种 profile 的实际输出、variant/码流表示不匹配、Require 拒绝错误实现、Prefer 同格式回退、SVT 初始化失败后的清理和后续重试。原有 H264 双档、B 帧重排、RTP32 回绕、输入尺寸变化、队列恢复及 RTP 接收至转码测试继续保留。测试源码见 [test_multicodec.cpp](../service/TranscodeService/Test/test_multicodec.cpp)。

本机 libavcodec 版本为 63.1.102，SVT-AV1 版本为 4.2.0；独立测试和 ASan/UBSan 均通过；主程序构建及 10 组相关 CTest 回归通过。新增 multicodec 测试包含 19 个用例。libaom 编解码库不在本机 FFmpeg 中，相关候选只完成实现及缺失过滤，未完成真实编码验证。没有新增浏览器/协议对端互通、硬件或容量/时延验收。

相关检查命令：

```sh
cmake --build build/video-transcoder -j 4
ctest --test-dir build/video-transcoder --output-on-failure
cmake --build build/webrtc-integration -j 4
ctest --test-dir build/webrtc-integration --output-on-failure -R '^(transcode_domain|video_transcoder|multicodec|server_app|stream_services|webrtc_service|test_rtp_input|memory_model|server_launcher|ConferenceMixFramework)$'
```

后续每次实现继续报告具体 codec、方向、后端、平台/配置、回退规则和已验证的业务链路。
