# H264 packet buffer test

This standalone module validates and benchmarks the path below without linking
the RTSP server:

```text
RTP payload -> H264RtpPayloadParser -> H264PacketBuffer -> H264AccessUnit
```

Build from the server's normal build directory:

```bash
cd build
cmake ..
cmake --build . --target h264_packet_buffer_perf -j8
./Common/Depacketizer/Test/h264_packet_buffer_perf --validate-only
```

Or build this module independently with optimizations enabled:

```bash
cmake -S Common/Depacketizer/Test -B build-h264-perf -DCMAKE_BUILD_TYPE=Release
cmake --build build-h264-perf -j
```

Run correctness validation only:

```bash
ctest --test-dir build-h264-perf --output-on-failure
# or
./build-h264-perf/h264_packet_buffer_perf --validate-only
```

Run the benchmark:

```bash
./build-h264-perf/h264_packet_buffer_perf \
  --frames 5000 \
  --packets-per-frame 100 \
  --payload-size 1200 \
  --reorder-window 8 \
  --warmup 200
```

`--reorder-window 1` is ordered input. A larger value reverses groups of
middle/end packets while retaining the first FU-A fragment, simulating bounded
RTP reordering. The process exits non-zero on any correctness or frame-count
failure.
