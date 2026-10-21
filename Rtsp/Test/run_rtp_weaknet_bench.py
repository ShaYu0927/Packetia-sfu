#!/usr/bin/env python3
"""Run the C++ production reorder queue through deterministic and local UDP traces."""
import argparse
import csv
import datetime
import hashlib
import json
import pathlib
import platform
import statistics
import subprocess
import time

SCENARIOS = {
    "baseline": "正常音频，50 包/秒，无故障",
    "audio_jitter": "音频：10–50 ms 单向延迟、1% 丢包、2% 重复",
    "video_moderate": "1000 包/秒：10–90 ms 单向延迟、3% 丢包、2% 重复",
    "video_severe": "1000 包/秒：20–200 ms 单向延迟、10% 丢包、5% 重复",
    "delayed_repair": "1000 包/秒：5% 包额外延迟 180 ms 后补发",
    "burst_loss": "1000 包/秒：连续丢失 200 包，之后恢复",
    "owner_stall": "1000 包/秒：接收 owner 阻塞 250 ms，另丢 1 包",
    "large_gap": "1000 包/秒：连续丢失 1200 包，超过默认序号窗口",
    "high_rate": "5000 包/秒：10–50 ms 单向延迟、1% 丢包",
}


def command(*args):
    return subprocess.check_output(args, text=True).strip()


def flattened(row, prefix=""):
    result = {}
    for key, value in row.items():
        name = prefix + key
        if isinstance(value, dict):
            result.update(flattened(value, name + "."))
        else:
            result[name] = value
    return result


def measured_findings(results, aggregate):
    """Explain measured tradeoffs without treating output order as recovery success."""
    groups = {(r["scenario"], r["max_cache"], r["wait_ms"]): r for r in aggregate}
    moderate = groups["video_moderate", 50, 100]
    moderate_large = groups["video_moderate", 128, 100]
    severe = groups["video_severe", 50, 100]
    severe_large = groups["video_severe", 128, 200]
    fast = groups["high_rate", 50, 100]
    fast_large = groups["high_rate", 256, 100]
    stall = groups["owner_stall", 50, 100]
    gap = groups["large_gap", 50, 100]
    gap_trials = [r for r in results if r["mode"] == "udp" and r["scenario"] == "large_gap"]
    delivery_gap = sum(r["local_delivery_gap"] for r in results if r["mode"] == "udp")
    lines = ["", "## 实测解读", "",
        f"- 默认参数在中等抖动下输出率为 {moderate['output_pct']:.2f}%。容量增到 128 后为 "
        f"{moderate_large['output_pct']:.2f}%，源到输出 P95 则从 {moderate['total_p95_ms']:.2f} ms "
        f"升至 {moderate_large['total_p95_ms']:.2f} ms。本组扩大容量没有增加输出，只增加了等待。",
        f"- 严重抖动下默认参数输出率为 {severe['output_pct']:.2f}%；同时调到 128 包/200 ms 后为 "
        f"{severe_large['output_pct']:.2f}%，排队 P95 从 {severe['queue_p95_ms']:.2f} ms 升至 "
        f"{severe_large['queue_p95_ms']:.2f} ms。这是容量与期限一起变化的对照，不能归因于单一参数。",
        f"- 5000 包/秒下，仅把容量从 50 改到 256，输出率从 {fast['output_pct']:.2f}% 提升至 "
        f"{fast_large['output_pct']:.2f}%；排队 P95 从 {fast['queue_p95_ms']:.2f} ms 升至 "
        f"{fast_large['queue_p95_ms']:.2f} ms。固定 50 包不能覆盖所有包速率；"
        "5000 包/秒的 40 ms 乱序跨度已经约为 200 包。",
        f"- owner 阻塞时，队列峰值只有 {stall['peak']} 包，实际排队最大值仍为 "
        f"{stall['queue_max_ms']:.2f} ms，入队前最大积压 {stall['prequeue_max_ms']:.2f} ms。"
        "限制排序缓存不能限制 socket 和调度积压；超时处理需要 owner 得到执行机会。",
        f"- 连续丢 1200 包后，{gap['tail_recovered']}/{len(gap_trials)} 次试验恢复尾部输出；"
        f"重同步 {gap['resyncs']} 次，{gap['out_of_window']} 个包被窗口拒绝。"
        + ("前两个探测包被丢弃，第三个起恢复；这不等于验证了视频解码恢复。"
           if gap['tail_recovered'] == len(gap_trials) else
           "仍有未恢复场景，不能以缓存为零、排队耗时为零判断恢复成功。"),
        "", "### 输出损失对账", "",
        "以下计数按种子合计。额外未输出 = 源包 − 注入丢包 − 唯一输出包。"
        + ("本次 UDP 投递差额为零，因此该列反映队列策略导致的未输出。" if delivery_gap == 0 else
           f"本次 UDP 投递差额为 {delivery_gap}，该列也可能包含本机未投递包。") +
        "它包含迟到与窗口拒绝，不把重复数据报误当作丢失的唯一源包。", "",
        "| 场景 | 容量/期限 | 源包 | 注入丢包 | 额外未输出 |",
        "|---|---:|---:|---:|---:|"]
    for group in aggregate:
        rows = [r for r in results if r["mode"] == "udp" and r["scenario"] == group["scenario"]
                and r["max_cache"] == group["max_cache"] and r["wait_ms"] == group["wait_ms"]]
        dropped = sum(r["network_dropped"] for r in rows)
        extra = group["generated"] - dropped - group["emitted"]
        lines.append(f"| {group['scenario']} | {group['max_cache']}/{group['wait_ms']} | "
                     f"{group['generated']} | {dropped} | {extra} |")
    lines += ["", "每种配置仅重复三个种子、每次真实发包三秒，属于可复现的边界与参数对照试验；"
              "没有测出节点并发容量、长期稳定性或公网延迟保证。"]
    return lines


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--seeds", default="11,29,47")
    parser.add_argument("--udp-duration-ms", type=int, default=3000)
    parser.add_argument("--simulated-duration-ms", type=int, default=10000)
    args = parser.parse_args()
    binary = args.binary.resolve()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    (out / "samples").mkdir(exist_ok=True)
    seeds = [int(seed) for seed in args.seeds.split(",")]
    environment = {
        "started_at": datetime.datetime.now(datetime.timezone(datetime.timedelta(hours=8))).isoformat(),
        "os": command("sw_vers", "-productVersion") if platform.system() == "Darwin" else platform.system(),
        "architecture": platform.machine(),
        "compiler": command("c++", "--version").splitlines()[0],
        "benchmark_optimization": "CMake target -O2; not sanitizer-instrumented",
        "binary": str(binary), "seeds": seeds,
        "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "source_sha256": {name: hashlib.sha256(pathlib.Path(name).read_bytes()).hexdigest()
                          for name in ("Rtsp/RtpPacketReorderQueue.h", "Rtsp/Test/rtp_weaknet_bench.cpp",
                                       "Rtsp/Test/run_rtp_weaknet_bench.py")},
        "simulated_duration_ms": args.simulated_duration_ms, "udp_duration_ms": args.udp_duration_ms,
    }
    if platform.system() == "Darwin":
        for key in ("hw.model", "hw.ncpu", "hw.memsize", "machdep.cpu.brand_string"):
            environment[key] = command("sysctl", "-n", key)
    (out / "environment.json").write_text(json.dumps(environment, ensure_ascii=False, indent=2) + "\n")
    configs = [(scenario, 50, 100) for scenario in SCENARIOS]
    configs += [("video_moderate", 128, 100), ("video_severe", 128, 200), ("high_rate", 256, 100)]
    results = []
    started = time.monotonic()
    with (out / "trials.jsonl").open("w") as raw:
        for mode in ("simulated", "udp"):
            duration = args.udp_duration_ms if mode == "udp" else args.simulated_duration_ms
            for scenario, cache, wait in configs:
                for seed in seeds:
                    name = f"{mode}-{scenario}-c{cache}-w{wait}-s{seed}"
                    cmd = [str(binary), "--mode", mode, "--scenario", scenario, "--seed", str(seed),
                           "--duration-ms", str(duration), "--max-cache", str(cache), "--wait-ms", str(wait),
                           "--samples", str(out / "samples" / (name + ".csv"))]
                    completed = subprocess.run(cmd, capture_output=True, text=True, timeout=duration / 1000 + 20)
                    if not completed.stdout.strip():
                        raise RuntimeError(f"{name}: {completed.stderr}")
                    row = json.loads(completed.stdout)
                    row["process_returncode"] = completed.returncode
                    raw.write(json.dumps(row, ensure_ascii=False) + "\n")
                    raw.flush()
                    results.append(row)
                    print(f"{name}: emitted={row['emitted']}/{row['generated']} "
                          f"peak={row['peak_buffered']} wait_max={row['queue_wait_ms']['max']} ms "
                          f"ok={row['invariants_ok']}", flush=True)
    flat = [flattened(row) for row in results]
    fields = sorted({key for row in flat for key in row})
    with (out / "trials.csv").open("w", newline="") as file:
        writer = csv.DictWriter(file, fieldnames=fields, lineterminator="\n")
        writer.writeheader()
        writer.writerows(flat)
    failed = [row for row in results if row["process_returncode"] or not row["invariants_ok"]
              or not row["recovery_ok"]]
    local_loss = sum(row.get("local_delivery_gap", 0) for row in results if row["mode"] == "udp")
    environment["elapsed_wall_seconds"] = round(time.monotonic() - started, 3)
    environment["trials"] = len(results)
    environment["failed_invariants"] = len(failed)
    environment["udp_local_delivery_gap"] = local_loss
    (out / "environment.json").write_text(json.dumps(environment, ensure_ascii=False, indent=2) + "\n")

    def median(rows, field, stat):
        return statistics.median(row[field][stat] for row in rows)

    lines = ["# macOS RTP 排序队列弱网实测", "",
        f"时间：{environment['started_at']}。机器：{environment.get('machdep.cpu.brand_string', '')}、"
        f"{environment.get('hw.model', '')}、{environment['architecture']}、macOS {environment['os']}。",
        f"种子：{seeds}；共 {len(results)} 次试验，耗时 {environment['elapsed_wall_seconds']:.1f} 秒。",
        "", "## 方法与指标", "",
        "直接调用仓库中的 `RtpPacketReorderQueue`。每个队列对应单个 SSRC，默认容量 50 包、等待 100 ms、"
        "向前窗口 1000 个序号、周期检查约 10 ms；每包 1200 字节，初始序号 65500，所有试验跨越序号回绕。",
        "自动重同步要求正常输出停滞至少 500 ms、旧缓存为空，并在 100 ms 内观察到三个向前超窗的连续序号；"
        "前两个探测包不保留，第三个起恢复。迟到包和半周期歧义不触发重同步。",
        "",
        f"确定性试验每次覆盖 {args.simulated_duration_ms / 1000:g} 秒虚拟时间，使用注入时间点精确重放；"
        f"UDP 试验每次发送 {args.udp_duration_ms / 1000:g} 秒，随后自然排空等待期限。"
        "UDP 使用真实 `127.0.0.1` 数据报，发送线程通过用户态调度注入丢包、延迟和重复，接收线程运行队列及周期检查。"
        "不调用强制排空，不修改系统网络设置，不向外部服务器发包。首尾各 16 个源包不注入随机故障，固定序号起点并观察尾部恢复。",
        "",
        "- 输出率：输出的唯一源包 / 生成源包；包含注入的网络丢包，不能直接当作队列误丢率。",
        "- 排队 P95：每次试验输出包的队列等待 P95，再取多个种子的中位数。排队最大值：多个种子的最大值。",
        "- 源到输出 P95：从计划源包产生到队列输出，包括注入延迟、发送调度、UDP/owner 等待及排序等待；不含编解码和播放器。",
        "- UDP 到队列最大值：从用户态实际 sendto 前计时到接收方提交队列，包含内核 socket 缓冲和接收 owner 调度。",
        "- 时延分布只统计已输出包；迟到/窗口拒绝等丢弃数量另列，防止只看时延掩盖损失。",
        "- 延迟是单向注入值；180 ms 补发是模拟晚到包，不等于测试了真实 RTCP/NACK 往返。",
        "", "## 真实 UDP 结果", "",
        "计数为种子合计，峰值/最大耗时取最大；其余 P95 按上述口径聚合。所有时延单位 ms。", "",
        "| 场景 | 包/秒 | 容量/期限 | 输出/源包 | 输出率 | 峰值包数 | 排队P95 | 排队最大 | 源到输出P95 | UDP到队列最大 | 超时/溢出 | 窗口拒绝 | 重同步 |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
    aggregate = []
    for scenario, cache, wait in configs:
        rows = [r for r in results if r["mode"] == "udp" and r["scenario"] == scenario and
                r["max_cache"] == cache and r["wait_ms"] == wait]
        generated = sum(r["generated"] for r in rows)
        emitted = sum(r["emitted"] for r in rows)
        group = dict(scenario=scenario, max_cache=cache, wait_ms=wait, generated=generated, emitted=emitted,
                     output_pct=100 * emitted / generated, peak=max(r["peak_buffered"] for r in rows),
                     queue_p95_ms=median(rows, "queue_wait_ms", "p95"),
                     queue_max_ms=max(r["queue_wait_ms"]["max"] for r in rows),
                     total_p95_ms=median(rows, "source_to_output_ms", "p95"),
                     prequeue_max_ms=max(r["udp_send_to_queue_ms"]["max"] for r in rows),
                     timeouts=sum(r["timeouts"] for r in rows), overflows=sum(r["overflows"] for r in rows),
                     out_of_window=sum(r["out_of_window"] for r in rows),
                     resyncs=sum(r["resyncs"] for r in rows),
                     resync_attempts=sum(r["resync_attempts"] for r in rows),
                     tail_recovered=sum(r["tail_emitted"] for r in rows))
        aggregate.append(group)
        lines.append(f"| {scenario} | {rows[0]['pps']} | {cache}/{wait} | {emitted}/{generated} | "
                     f"{group['output_pct']:.2f}% | {group['peak']} | {group['queue_p95_ms']:.2f} | "
                     f"{group['queue_max_ms']:.2f} | {group['total_p95_ms']:.2f} | {group['prequeue_max_ms']:.2f} | "
                     f"{group['timeouts']}/{group['overflows']} | {group['out_of_window']} | {group['resyncs']} |")
    lines += measured_findings(results, aggregate)
    lines += ["", "## 故障注入参数", ""]
    lines += [f"- `{key}`：{value}。" for key, value in SCENARIOS.items()]
    lines += ["", "## 不变量与已知边界", "",
        f"试验失败 {len(failed)} 次。检查输出序号严格递增且不重复、外部可见缓存不超配置、"
        "试验结束缓存自然排空、接收数能与输出/重复/迟到/窗口拒绝逐项对账。",
        "`large_gap` 另要求恰好一次重同步且尾部恢复输出，否则进程返回非零；不以排序不变量通过代替恢复验收。",
        f"真实 UDP 的成功发送与接收差额合计 {local_loss} 个数据报；每次的发送错误、接收错误、socket 接收缓冲大小均保存在原始数据中。",
        "",
        "- `large_gap` 刻意连续丢失 1200 包，超过当前向前窗口；尾部恢复及验证耗时见 `tail_emitted`、"
        "`resume_arrival_to_output_ms`。不调用强制 reset，保留累计统计。",
        "- 自动恢复仅处理向前距离小于半周期的序号；后退重启、跨越半周期或不能形成连续探测的流，仍可能需要会话层重建。"
        "队列不验证 SRTP、SSRC、Payload Type 或视频帧依赖，这些仍由调用层处理。",
        "- `owner_stall` 刻意阻塞接收线程 250 ms。检查暂停期间无法运行，100 ms 只是队列等待策略，不能约束线程阻塞或其前面的 socket 积压。",
        "- 容量/期限对照使用相同种子的网络轨迹；真实 UDP 仍有操作系统调度差异。调大容量可能减少提前跳过，调大期限会增加等待。",
        "- 本次是通用 RTP 排序队列的本机实测，未穿过完整 SFU worker/转码/发送/浏览器链路，"
        "也不据此宣称 H264 帧完整性或真实公网网络已验收。合成 RTP payload 不包含可播放的视频。",
        "", "## 原始数据与复现", "",
        "- [逐次试验 CSV](trials.csv)", "- [完整统计 JSONL](trials.jsonl)",
        "- [机器与运行参数](environment.json)", "- `samples/`：每次试验的队列深度、最老包年龄及累计计数时间序列。",
        "", "从仓库根目录执行：", "", "```sh",
        "cmake -S . -B build/webrtc-integration",
        "cmake --build build/webrtc-integration --target rtp_weaknet_bench -j 4",
        "python3 Rtsp/Test/run_rtp_weaknet_bench.py \\",
        "  --binary build/webrtc-integration/Rtsp/Test/rtp_weaknet_bench \\",
        "  --output build/rtp-weaknet-results", "```", ""]
    (out / "report.md").write_text("\n".join(lines))
    (out / "aggregate.json").write_text(json.dumps(aggregate, ensure_ascii=False, indent=2) + "\n")
    print(f"Report: {out / 'report.md'}; failures={len(failed)}", flush=True)
    return bool(failed)


if __name__ == "__main__":
    raise SystemExit(main())
