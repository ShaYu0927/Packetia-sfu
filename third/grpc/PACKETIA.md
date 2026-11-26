# Packetia 的 gRPC 源码与构建

本目录直接保存 gRPC **v1.85.0** 的源码快照，以及 C++ 构建所需的 Abseil、c-ares、
Protobuf、RE2、zlib、grpc-proto、cel-c 共 7 个依赖。普通 clone Packetia 即可获得这些源码，
无需额外下载子模块。各仓库 URL 和完整提交号记录在 [PACKETIA_SOURCES.json](PACKETIA_SOURCES.json)。
依赖版本与 gRPC 上游提交固定的版本一致，各自的 LICENSE、版权及 NOTICE 文件保留在原位置。

快照通过 `git archive` 从指定提交导出，保留上游目录和文件，包括上游已有的测试夹具。
不包含上游 Git 历史、本机编译目录或本机安装包。BoringSSL、上游测试和其他可选功能所需的
额外子模块未导入；上游 `.gitmodules` 保留为来源信息，本目录在 Packetia 中不是 Git 子模块。
升级时应根据清单从对应仓库导出新版本，更新依赖提交号并重新验证，不在本目录运行子模块更新。

## 随项目编译

`PACKETIA_WITH_GRPC` 默认关闭。源码方式需要 CMake 3.22+、C++17 编译器和 OpenSSL 开发包，
项目其他依赖见根目录 README。在 macOS 或 Linux 上，从项目根目录执行：

```sh
cmake -S . -B build/agent-source \
  -DCMAKE_BUILD_TYPE=Release \
  -DPACKETIA_WITH_GRPC=ON \
  -DPACKETIA_GRPC_PROVIDER=source
cmake --build build/agent-source --target PacketiaControl --parallel 4
```

产物位于 `build/agent-source/third/grpc/`。源码方式构建静态库，按目标依赖编译，
关闭上游测试和其他语言的 gRPC 插件，保留 Packetia 自身测试。
zlib 会重命名源码中的配置头文件，因此构建时先将其复制到构建目录再配置，保持仓库源码不变。
gRPC 使用项目选中的同一套 OpenSSL，因此不需要 BoringSSL。
macOS 可增加 `-DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"`；
Debian/Ubuntu 可安装 `libssl-dev`。macOS、Linux 及不同 CPU 架构应使用各自的构建目录。

## 使用已安装的库

```sh
cmake -S . -B build/agent-package \
  -DPACKETIA_WITH_GRPC=ON \
  -DPACKETIA_GRPC_PROVIDER=package \
  -DPACKETIA_GRPC_ROOT="$PWD/build/grpc-v1.85.0/install"
cmake --build build/agent-package --target PacketiaControl --parallel 4
```

该目录需要包含匹配的 gRPC、Protobuf 库、头文件、CMake 配置和代码生成工具。
也可省略 `PACKETIA_GRPC_ROOT`，通过 `CMAKE_PREFIX_PATH` 或系统安装查找。
预编译库必须匹配目标平台、架构及 OpenSSL 等依赖；例如此前使用 OpenSSL 3 构建的库，
主项目也应选择对应的 OpenSSL。更换依赖时建议使用新的构建目录。

## 接口与验证

| 目标 | 用途 |
| --- | --- |
| `packetia_grpc` | 为业务代码提供 gRPC C++ 和 Protobuf 链接依赖 |
| `protobuf::protoc` | 生成消息的 C++ 代码 |
| `gRPC::grpc_cpp_plugin` | 生成 C++ RPC 接口 |

开启 `BUILD_TESTING` 后执行：

```sh
cmake --build build/agent-source --target test_grpc_smoke --parallel 4
ctest --test-dir build/agent-source -R '^GrpcSmokeTest$' --output-on-failure
```

测试使用所选工具生成代码，在本机回环地址的动态端口启动服务，并完成一次有超时限制的 RPC。
代码生成示例见 `cmake/grpc/test/CMakeLists.txt`。
Agent RPC 业务、专属调度线程和独立 Worker 尚未实现。

| 配置 | 默认值 | 含义 |
| --- | --- | --- |
| `PACKETIA_WITH_GRPC` | `OFF` | 启用编译依赖 |
| `PACKETIA_GRPC_PROVIDER` | `source` | `source` 编译源码，`package` 使用安装包 |
| `PACKETIA_GRPC_SOURCE_DIR` | `third/grpc` | 源码目录 |
| `PACKETIA_GRPC_ROOT` | 空 | 安装包模式的安装前缀 |

关闭 `PACKETIA_WITH_GRPC` 后不配置 gRPC 源码、不查找安装包，也不添加 RPC 冒烟测试。
旧的 `build/grpc/build.log` 是此前下载失败的日志；应查看实际构建目录对应的输出。

2026-11-26 在 macOS ARM64、CMake 4.4.3、AppleClang 21、OpenSSL 3.6.4 下对本源码快照
进行全新构建：`PacketiaControl` 编译通过，`GrpcSmokeTest` 和 `VoiceAgentUnitTest` 全部通过。
Linux 本轮未实机验证。
