# 双人录像合成实验

这个独立示例演示“多路视频解码 → 缩放与左右拼接 → 重新编码”，以及
“多路音频解码 → 统一采样格式 → 混音 → AAC 编码”。输出一个含单路视频和单路音频的 MP4。
现有 `Mp4Writer` 做容器封装；画面合成需要额外的解码、滤镜和编码处理。

在项目根目录执行。Ubuntu 需要系统 Python 3 和带 libx264/AAC 支持的 FFmpeg：

```sh
sudo apt install python3 ffmpeg
python3 service/RecordService/examples/compose_recordings.py \
  --demo --duration 10 --output build/composition-demo.mp4
```

左侧为动态测试画面，右侧为彩条。音频同时包含 440 Hz 和 880 Hz 的测试音。
输出为 1280×360、25 fps、H264 视频和 48 kHz 双声道 AAC 音频，可以用普通播放器打开。
输出文件已存在时拒绝覆盖，可指定新文件名再次运行。

使用两份已经录制完成的 MP4：

```sh
python3 service/RecordService/examples/compose_recordings.py \
  --left recordings/person-a.mp4 --right recordings/person-b.mp4 \
  --duration 30 --output build/two-people.mp4
```

每份输入使用第一条视频轨和第一条音频轨；没有音频时补静音。画面等比例缩放，空余区域补黑边。
输出在较短视频结束或达到 `--duration` 时停止。混音采用归一化，单路音频缺失时另一侧可能更轻。

为了便于实验，各输入的视频和音频分别从零对齐。它没有使用会议的真实采集时间，
因此不能用于验证不同起录时间、原始音视频偏移或时钟漂移下的同步效果。
这是离线合成预览，不会自动合成会议，也没有接入实时房间、录像任务或 NAS 归档。

后续实时接入的位置是：`EncodedFrameRouter → 合成服务 → 新的 H264/AAC 编码帧 → RecordingService`。
届时需要新增会议级任务、统一时间轴、参与者加入退出、独立处理队列，以及合成输出的独立流标识
和订阅过滤，避免合成服务再次消费自己的输出。多视频解码与重新编码需要额外 CPU 或 GPU 资源。
