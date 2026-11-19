# 从仓库源码编译 FFmpeg

仓库内的 `ffmpeg-9.0.1/` 是 FFmpeg 9.0.1 上游源码。
`scripts/build_ffmpeg.py` 负责选择组件、调用上游 `configure → make → make install-*`，
`cmake/ffmpeg/CMakeLists.txt` 负责将安装结果接入 Packetia。
脚本当前支持 macOS / Linux 本机编译，不下载 FFmpeg 或外部 codec、不写入系统安装目录。
安装阶段选择库、头文件和可选工具对应的上游目标，跳过未收录的完整文档与示例安装。

## 目录与构建过程

```text
third/ffmpeg/ffmpeg-9.0.1/  上游源码，包含真正的 AAC/H264 等算法实现
             │
scripts/build_ffmpeg.py    组件选择、依赖检查、构建与安装
             │
build/ffmpeg/work/         configure 结果、对象文件、构建日志
build/ffmpeg/install/      头文件、动态库、pkg-config 元数据、可选命令行工具
             │
cmake/ffmpeg/              根据 PACKETIA_FFMPEG_ROOT 查找同一套头文件与库
             │
Packetia 录制 / 转码 / 图像缩放模块
```

构建目录已被 Git 忽略。修改源码后重新执行同一条构建命令，`make` 会检查源文件变化并增量编译，
随后重新安装；组件选择、编译环境或 `configure` 内容变化时，脚本清理旧生成文件并重新配置。
同一个输出目录应由一个构建进程使用。

## 基础构建

需要 Python 3.9+、C 编译器和 GNU make；项目通过 `pkg-config` 查找安装结果。
Linux x86 汇编优化还需要 NASM，也可显式传入 `--disable-asm` 构建无汇编版本。
macOS ARM64 使用系统 Clang 的汇编器，无需 NASM。

Ubuntu / Debian：

```sh
sudo apt install build-essential python3 pkg-config nasm
```

macOS 安装 Xcode Command Line Tools，并准备 `pkg-config`：

```sh
brew install pkgconf
```

在仓库根目录执行：

```sh
python3 scripts/build_ffmpeg.py --jobs 4
```

基础构建生成 `libavformat`、`libavcodec`、`libavutil`、`libswscale` 和 `libswresample` 动态库。
它裁剪到项目常用的组件：

| 类型 | 主要启用项 |
| --- | --- |
| 内置解码 | H264、HEVC、VP8、VP9、AAC、Opus、PCM |
| 内置编码 | AAC、PCM、原始视频 |
| 容器 | MP4/MOV、ADTS、Ogg、Matroska/WebM、IVF，以及部分裸流读取 |
| 协议 | 本地文件和 pipe；网络协议由 Packetia 自己处理 |
| 位流处理 | H264/HEVC Annex B 转换、AAC ADTS 转换、extradata 提取等依赖项 |

基础构建不包含 H264/H265/VP8/VP9/AV1 软件编码器，AV1 的软件解码也需要外部库。
`VideoCodecFactory::Capabilities()` 会根据实际编入的实现报告可用性。
这套裁剪不是通用 FFmpeg 发行包；需要新格式时，应同时调整脚本组件和应用适配。

## 加入软件编解码库

外部 codec 库需先安装开发包。脚本通过它们的 `.pc` 文件检查依赖，FFmpeg 自身仍从仓库编译。

| 参数 | 开发包的 pkg-config 名称 | 提供的能力 |
| --- | --- | --- |
| `--codec-library x264` | `x264` | H264 编码，需要 `--enable-gpl` |
| `--codec-library x265` | `x265` | HEVC 编码，需要 `--enable-gpl` |
| `--codec-library vpx` | `vpx` | VP8/VP9 编码和外部解码实现 |
| `--codec-library dav1d` | `dav1d` | AV1 解码 |
| `--codec-library aom` | `aom` | AV1 编码与解码 |
| `--codec-library svtav1` | `SvtAv1Enc` | AV1 编码 |
| `--codec-library opus` | `opus` | 外部 Opus 编码与解码 |

例如，macOS 可以安装 `brew install x264 x265 libvpx dav1d`；
Ubuntu / Debian 对应 `libx264-dev libx265-dev libvpx-dev libdav1d-dev`。
启用 x264/x265 会使 FFmpeg 构建采用 GPL 许可，脚本要求显式选择 `--enable-gpl`；
未启用这些库的基础构建沿用上游配置决定的许可，许可证原文保留在源码目录中。

编译 H264、HEVC、VP8/VP9 编码与 AV1 解码，并生成命令行工具：

```sh
python3 scripts/build_ffmpeg.py --jobs 4 --programs --enable-gpl \
  --codec-library x264 --codec-library x265 \
  --codec-library vpx --codec-library dav1d
```

`--programs` 生成 `install/bin/ffmpeg`、`ffprobe`，同时启用录制测试所需的 lavfi、
测试画面、正弦音频、缩放和格式转换等组件。它不会生成 ffplay。
AV1 编码需另外增加 `aom` 或 `svtav1`，并安装对应开发包。

## 让 Packetia 使用编译结果

```sh
cmake -S . -B build/ffmpeg-app \
  -DPACKETIA_FFMPEG_ROOT="$PWD/build/ffmpeg/install"
cmake --build build/ffmpeg-app --target Packetia --parallel 4
```

项目其他依赖仍按根 README 准备。指定根目录后，四个直接依赖的 FFmpeg 库及头文件必须来自该安装，
缺失 `.pc` 文件或库会报错；不会用系统 FFmpeg 补齐其中一部分。
`libswscale` 也通过统一的 `packetia_ffmpeg` 目标传递，避免缩放模块单独找到另一套版本。

不传 `PACKETIA_FFMPEG_ROOT` 时仍使用系统开发包。在同一个 CMake 构建目录切换安装路径时，
相关库查找缓存会失效并重新发现。自编译流程提供动态库；外部 codec 的运行库也需在运行环境可用。
安装目录的绝对路径会用于本机运行，不应把它当作可直接复制到任意机器的独立发布包。

## 检查结果与排错

```sh
python3 scripts/build_ffmpeg.py --dry-run
python3 scripts/test_ffmpeg_build.py
build/ffmpeg/install/bin/ffmpeg -version
build/ffmpeg/install/bin/ffmpeg -encoders
```

最后两条命令要求构建时使用 `--programs`。构建日志保存在 `work/build.log`，
`work/packetia-config.json` 和 `install/packetia-build.json` 记录源码版本、配置参数与编译环境。
依赖找不到时，先用 `pkg-config --modversion x264` 等命令检查；非标准安装可设置 `PKG_CONFIG_PATH`。

独立转码验证：

```sh
cmake -S service/TranscodeService/Test -B build/transcode-source \
  -DPACKETIA_FFMPEG_ROOT="$PWD/build/ffmpeg/install"
cmake --build build/transcode-source --parallel 4
PACKETIA_LOG_MODE=console ctest --test-dir build/transcode-source --output-on-failure
```

录制验证需要 `--programs --codec-library x264 --enable-gpl`：

```sh
cmake -S service/RecordService/Test -B build/recording-source \
  -DPACKETIA_FFMPEG_ROOT="$PWD/build/ffmpeg/install"
cmake --build build/recording-source --parallel 4
PACKETIA_LOG_MODE=console ctest --test-dir build/recording-source --output-on-failure
```

指定本地安装时，录制测试只使用该安装的 `bin/ffmpeg`，不回退到系统命令行工具。
编解码测试会根据实际可用实现执行或跳过对应场景；没有安装某个 codec 不能被表述为已验证该能力。

## 本次验证范围

在 macOS ARM64 上完成了以下验证：

- 基础版及启用 x264、x265、vpx、dav1d 和命令行工具的版本均编译、安装成功。
- Packetia 主程序使用自编译安装构建成功；动态库路径检查确认来自指定安装目录。
- 构建配置测试 5 项通过，覆盖目录隔离、GPL 显式选择、安装路径切换和禁止系统回退。
- 转码 CTest 3 项通过；内部用例 30 项通过、8 项跳过，实际验证了 H264、HEVC、VP8、VP9 编码。
- 录制 CTest 8 项通过，媒体样本由自编译的 `ffmpeg` 生成。

本次未启用 AV1 编码器，不能据此认为完整五编码格式链路已经验证；Linux 构建尚未实测。
主程序构建环境未发现 libsrtp2，因此此次构建也不代表已验证 WebRTC 媒体运行链路。

## 上游构建文件来源

原始源码导入时，根 `.gitignore` 的 `Makefile` / `doc/` 规则遗漏了上游构建文件。
此次补齐 FFmpeg `n9.0.1` 标签中的 Makefile 和安装所需的 `doc/ffprobe.xsd`，
并增加只针对这些源码文件的忽略例外。已有 10227 个上游文件与该标签的 Git blob 一致，
补齐文件也逐个验证了 blob SHA-1；没有改写编解码算法。

上游：<https://github.com/FFmpeg/FFmpeg/tree/n9.0.1>，
对应提交：`bf1b838f2ab88b4f8fd83443325c782ea0e0f7fa`，
Git tree：`68fad6870d0f112fcf4e385d798bf268b2b938b0`。
文档正文未完整补入，当前脚本始终使用 `--disable-doc`。
