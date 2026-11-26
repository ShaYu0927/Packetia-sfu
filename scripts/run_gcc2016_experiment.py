#!/usr/bin/env python3
"""Run the standalone GCC 2016 reproduction; requires only Python's standard library."""

import argparse
import csv
import json
import math
from pathlib import Path
import statistics
import subprocess


ROOT = Path(__file__).resolve().parents[1]


def percentile(values, probability):
    ordered = sorted(values)
    position = (len(ordered) - 1) * probability
    lower = math.floor(position)
    upper = math.ceil(position)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def read_trace(path):
    with path.open(newline="") as source:
        rows = list(csv.DictReader(source))
    for row in rows:
        for field in row:
            if field not in ("signal", "rate_state"):
                row[field] = float(row[field])
                if not math.isfinite(row[field]):
                    raise ValueError(f"non-finite {field} in {path}")
        if not 10000 <= row["target_bps"] <= 2000000:
            raise ValueError(f"target outside configured bounds in {path}")
        if row["queue_ms"] < 0 or row["threshold_ms"] <= 0:
            raise ValueError(f"invalid queue/threshold in {path}")
    if not rows or rows[-1]["time_ms"] <= rows[0]["time_ms"]:
        raise ValueError(f"empty or incomplete trace: {path}")
    return rows


def summarize(rows, begin, end, capacity):
    by_time = {row["time_ms"]: row for row in rows}
    first, last = by_time[begin], by_time[end]
    samples = [row for row in rows if begin <= row["time_ms"] < end]
    duration = end - begin
    delivered_bps = (last["received_bytes"] - first["received_bytes"]) * 8000 / duration
    sent = last["sent_bytes"] - first["sent_bytes"]
    lost = last["lost_bytes"] - first["lost_bytes"]
    return {
        "begin_ms": begin,
        "end_ms": end,
        "capacity_bps": capacity,
        "mean_target_bps": statistics.mean(row["target_bps"] for row in samples),
        "delivered_bps": delivered_bps,
        "utilization": delivered_bps / capacity,
        "queue_p50_ms": percentile([row["queue_ms"] for row in samples], 0.5),
        "queue_p95_ms": percentile([row["queue_ms"] for row in samples], 0.95),
        "enqueue_byte_drop_fraction": lost / sent if sent else 0,
        "remb_notifications": int(last["remb_count"] - first["remb_count"]),
        "overuse_frames": int(last["overuse_frames"] - first["overuse_frames"]),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "build-gcc2016/gcc2016_experiment")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "build-gcc2016/artifacts")
    args = parser.parse_args()
    binary = args.binary.resolve()
    if not binary.is_file():
        parser.error("provide --binary pointing to a built gcc2016_experiment; its source is not included in this checkout")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    results = {}
    for scenario, duration in (("variable", 200000), ("constant", 60000), ("jitter", 60000)):
        trace = args.output_dir / f"{scenario}.csv"
        subprocess.run([str(binary), "--scenario", scenario, "--duration-ms", str(duration),
                        "--output", str(trace)], check=True)
        rows = read_trace(trace)
        phases = ([(0, 50000, 1000000), (50000, 100000, 2500000),
                   (100000, 150000, 500000), (150000, 200000, 1000000)]
                  if scenario == "variable" else [(0, duration, 1000000)])
        results[scenario] = {"samples": len(rows), "phases": [summarize(rows, *phase) for phase in phases]}
        if scenario == "variable":
            crossing = next((row["time_ms"] for row in rows
                             if row["time_ms"] >= 100000 and row["target_bps"] <= 550000), None)
            results[scenario]["first_target_at_or_below_550kbps_after_drop_ms"] = (
                crossing - 100000 if crossing is not None else None)

    (args.output_dir / "summary.json").write_text(json.dumps(results, indent=2) + "\n")
    lines = [
        "# GCC 2016 algorithm reproduction experiment", "",
        "Deterministic C++ packet-queue simulation; this is not a browser/public-network benchmark.", "",
        "Source: Carlucci et al., MMSys 2016, https://doi.org/10.1145/2910017.2910605.", "",
        "| Scenario | Time (s) | Capacity (kbps) | Mean target (kbps) | Delivered (kbps) | Utilization | Queue P50/P95 (ms) | Enqueue byte drops |",
        "|---|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for scenario, result in results.items():
        for phase in result["phases"]:
            lines.append(
                f"| {scenario} | {phase['begin_ms']/1000:.0f}-{phase['end_ms']/1000:.0f} | "
                f"{phase['capacity_bps']/1000:.0f} | {phase['mean_target_bps']/1000:.1f} | "
                f"{phase['delivered_bps']/1000:.1f} | {phase['utilization']:.1%} | "
                f"{phase['queue_p50_ms']:.1f}/{phase['queue_p95_ms']:.1f} | "
                f"{phase['enqueue_byte_drop_fraction']:.3%} |")
    delay = results["variable"]["first_target_at_or_below_550kbps_after_drop_ms"]
    lines += ["", f"First logged target <= 550 kbps after the 2.5 -> 0.5 Mbps drop: {delay} ms.",
              "Logging granularity is 100 ms; this is a threshold crossing, not a settling-time guarantee.",
              "", "## Model and measurement limits", "",
              "- 30 fps ideal encoder; RTP-like payloads up to 1200 bytes; 1 ms link service steps.",
              "- FIFO drop-tail queue, budget 300 ms at current capacity; 25 ms forward/reverse propagation.",
              "- Jitter scenario adds a deterministic +/-5 ms sinusoidal delay; it is not a general wireless model.",
              "- Target range 10-2000 kbps. A 2.5 Mbps link cannot be filled by this source's 2 Mbps limit.",
              "- Completed video frames feed the receiver; receive-rate accounting uses completed frames only.",
              "- RTCP loss is an enqueue-drop oracle, with 1 second reports; there is no real RTP/RTCP parser.",
              "- No real encoder adaptation lag, pacer queue, retransmission, FEC, CPU scheduling, or TCP competition.",
              "- Queue statistics sample true simulated forward-link queue delay every 100 ms, not measured RTT.",
              "- Byte drops are recorded at enqueue; bytes still queued/in flight at experiment end are not counted as loss.",
              "- Summary includes startup and capacity transitions. Parameter assumptions are documented in the module README.",
              "- Original-paper utilization, latency and fairness results have not been reproduced by this model.",
              "", "Raw traces: variable.csv, constant.csv, jitter.csv. Machine-readable summary: summary.json.", ""]
    report = args.output_dir / "REPORT.md"
    report.write_text("\n".join(lines))
    print(report.resolve())
    print("\n".join(lines[6:14]))


if __name__ == "__main__":
    main()
