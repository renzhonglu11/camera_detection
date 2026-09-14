#!/usr/bin/env python3
"""Analyze one or more camera_detection T02 metrics directories."""

from __future__ import annotations

import argparse
import csv
import json
import math
from collections import Counter
from pathlib import Path
from statistics import mean
from typing import Any, Iterable


def percentile(values: list[float], quantile: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    position = (len(ordered) - 1) * quantile
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    fraction = position - lower
    return ordered[lower] * (1.0 - fraction) + ordered[upper] * fraction


def distribution(values: list[float]) -> dict[str, float | int | None]:
    return {
        "count": len(values),
        "p50_ms": percentile(values, 0.50),
        "p95_ms": percentile(values, 0.95),
        "p99_ms": percentile(values, 0.99),
    }


def read_csv(path: Path) -> list[dict[str, str]]:
    if not path.exists():
        raise FileNotFoundError(f"Missing metrics file: {path}")
    with path.open(newline="", encoding="utf-8") as source:
        return list(csv.DictReader(source))


def load_json(path: Path) -> dict[str, Any]:
    if not path.exists():
        raise FileNotFoundError(f"Missing metrics file: {path}")
    with path.open(encoding="utf-8") as source:
        return json.load(source)


def duration_ms(
    rows: Iterable[dict[str, str]], begin: str, end: str
) -> list[float]:
    durations: list[float] = []
    for row in rows:
        if row.get(begin, "") and row.get(end, ""):
            durations.append((float(row[end]) - float(row[begin])) / 1000.0)
    return durations


def analyze_run(run_dir: Path, warmup_frames: int) -> dict[str, Any]:
    frames = read_csv(run_dir / "frames.csv")
    pushes = read_csv(run_dir / "stream_pushes.csv")
    resources = read_csv(run_dir / "resources.csv")
    config = load_json(run_dir / "run_config.json")
    runtime = load_json(run_dir / "runtime_summary.json")

    measured_frames = frames[warmup_frames:]
    if not measured_frames:
        raise ValueError(
            f"{run_dir}: warmup of {warmup_frames} removes all frame samples"
        )

    measured_source_ids = {row["source_frame_id"] for row in measured_frames}
    measured_pushes = [
        row for row in pushes if row["source_frame_id"] in measured_source_ids
    ]

    first_us = float(measured_frames[0]["read_begin_us"])
    last_us = float(measured_frames[-1]["result_ready_us"])
    active_seconds = max((last_us - first_us) / 1_000_000.0, 0.0)
    throughput = len(measured_frames) / active_seconds if active_seconds else None

    stage_specs = {
        "read": ("read_begin_us", "read_end_us"),
        "detect": ("detect_begin_us", "detect_end_us"),
        "control": ("control_begin_us", "control_end_us"),
        "overlay": ("overlay_begin_us", "overlay_end_us"),
        "publish_call": ("publish_begin_us", "publish_end_us"),
        "result_age": ("received_at_us", "result_ready_us"),
        "replay_schedule_lateness": (
            "scheduled_release_us",
            "result_ready_us",
        ),
    }
    timings = {
        name: distribution(duration_ms(measured_frames, begin, end))
        for name, (begin, end) in stage_specs.items()
    }
    timings["stream_frame_age"] = distribution(
        [float(row["frame_age_us"]) / 1000.0 for row in measured_pushes]
    )

    rss_values = [int(row["rss_kb"]) for row in resources if int(row["rss_kb"]) >= 0]
    peak_values = [
        int(row["peak_rss_kb"])
        for row in resources
        if int(row["peak_rss_kb"]) >= 0
    ]
    publish_statuses = Counter(row["publish_status"] for row in measured_frames)
    source_ids_pushed = {row["source_frame_id"] for row in pushes}

    expected = int(config.get("expected_frame_count", 0))
    source_read = int(runtime["source_frames_read"])
    offline_missing = None
    if config.get("input_mode") == "file" and expected > 0:
        offline_missing = max(expected - source_read, 0)

    summary: dict[str, Any] = {
        "run_dir": str(run_dir),
        "config": config,
        "warmup_frames": warmup_frames,
        "sample_count": len(measured_frames),
        "active_processing_seconds": active_seconds,
        "processing_throughput_fps": throughput,
        "observed_frame_size": {
            "width": int(measured_frames[0]["frame_width"]),
            "height": int(measured_frames[0]["frame_height"]),
        },
        "timings": timings,
        "counts": {
            "source_frames_read": source_read,
            "offline_missing_frames": offline_missing,
            "camera_upstream_drops": None,
            "publish_status": dict(sorted(publish_statuses.items())),
            "unique_source_frames_pushed": len(source_ids_pushed),
            "stream_pushes": len(pushes),
            "latest_frame_overwrites": runtime["latest_frame_overwrites"],
            "duplicate_stream_pushes": runtime["duplicate_stream_pushes"],
            "failed_stream_pushes": runtime["failed_stream_pushes"],
            "metrics_rows_dropped": runtime["metrics_rows_dropped"],
        },
        "resources": {
            "sample_count": len(resources),
            "rss_initial_kb": rss_values[0] if rss_values else None,
            "rss_final_kb": rss_values[-1] if rss_values else None,
            "rss_peak_kb": max(peak_values) if peak_values else None,
            "rss_change_kb": (
                rss_values[-1] - rss_values[0] if len(rss_values) >= 2 else None
            ),
        },
        "runtime": runtime,
    }

    with (run_dir / "summary.json").open("w", encoding="utf-8") as output:
        json.dump(summary, output, indent=2, sort_keys=True)
        output.write("\n")
    write_markdown(run_dir / "summary.md", summary)
    return summary


def fmt(value: Any, digits: int = 3) -> str:
    if value is None:
        return "unknown"
    if isinstance(value, float):
        return f"{value:.{digits}f}"
    return str(value)


def write_markdown(path: Path, summary: dict[str, Any]) -> None:
    timings = summary["timings"]
    counts = summary["counts"]
    resources = summary["resources"]
    lines = [
        "# T02 benchmark summary",
        "",
        f"- Run: `{summary['run_dir']}`",
        f"- Samples after warmup: {summary['sample_count']}",
        f"- Processing throughput: {fmt(summary['processing_throughput_fps'])} FPS",
        f"- Observed frame size: {summary['observed_frame_size']['width']}×{summary['observed_frame_size']['height']}",
        f"- Metrics rows dropped: {counts['metrics_rows_dropped']}",
        "- Camera upstream drops: unknown (not observable through the current OpenCV interface)",
        "",
        "| Timing | Samples | P50 ms | P95 ms | P99 ms |",
        "| --- | ---: | ---: | ---: | ---: |",
    ]
    for name, values in timings.items():
        lines.append(
            f"| {name} | {values['count']} | {fmt(values['p50_ms'])} | "
            f"{fmt(values['p95_ms'])} | {fmt(values['p99_ms'])} |"
        )
    lines.extend(
        [
            "",
            "## Counts",
            "",
            f"- Source frames read: {counts['source_frames_read']}",
            f"- Offline missing frames: {fmt(counts['offline_missing_frames'], 0)}",
            f"- Stream pushes: {counts['stream_pushes']}",
            f"- Unique source frames pushed: {counts['unique_source_frames_pushed']}",
            f"- Latest-frame overwrites: {counts['latest_frame_overwrites']}",
            f"- Duplicate stream pushes: {counts['duplicate_stream_pushes']}",
            f"- Failed stream pushes: {counts['failed_stream_pushes']}",
            f"- Publish status: `{json.dumps(counts['publish_status'], sort_keys=True)}`",
            "",
            "## Resources",
            "",
            f"- Initial RSS: {fmt(resources['rss_initial_kb'], 0)} KiB",
            f"- Final RSS: {fmt(resources['rss_final_kb'], 0)} KiB",
            f"- Peak RSS: {fmt(resources['rss_peak_kb'], 0)} KiB",
            f"- RSS change: {fmt(resources['rss_change_kb'], 0)} KiB",
            "",
        ]
    )
    path.write_text("\n".join(lines), encoding="utf-8")


def write_aggregate(path: Path, summaries: list[dict[str, Any]]) -> None:
    throughputs = [
        summary["processing_throughput_fps"]
        for summary in summaries
        if summary["processing_throughput_fps"] is not None
    ]
    aggregate = {
        "run_count": len(summaries),
        "mean_processing_throughput_fps": mean(throughputs) if throughputs else None,
        "all_frame_counts_match": len(
            {summary["counts"]["source_frames_read"] for summary in summaries}
        )
        == 1,
        "total_metrics_rows_dropped": sum(
            summary["counts"]["metrics_rows_dropped"] for summary in summaries
        ),
        "runs": summaries,
    }
    with (path / "aggregate_summary.json").open("w", encoding="utf-8") as output:
        json.dump(aggregate, output, indent=2, sort_keys=True)
        output.write("\n")

    lines = [
        "# T02 aggregate benchmark",
        "",
        f"- Runs: {aggregate['run_count']}",
        f"- Mean processing throughput: {fmt(aggregate['mean_processing_throughput_fps'])} FPS",
        f"- All source-frame counts match: {str(aggregate['all_frame_counts_match']).lower()}",
        f"- Total metrics rows dropped: {aggregate['total_metrics_rows_dropped']}",
        "",
        "| Run | Replay | Samples | FPS | Detect P50 ms | Detect P95 ms | Result age P95 ms | Peak RSS KiB |",
        "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for summary in summaries:
        lines.append(
            f"| {Path(summary['run_dir']).name} | "
            f"{summary['config']['replay_mode']} | {summary['sample_count']} | "
            f"{fmt(summary['processing_throughput_fps'])} | "
            f"{fmt(summary['timings']['detect']['p50_ms'])} | "
            f"{fmt(summary['timings']['detect']['p95_ms'])} | "
            f"{fmt(summary['timings']['result_age']['p95_ms'])} | "
            f"{fmt(summary['resources']['rss_peak_kb'], 0)} |"
        )
    (path / "aggregate_summary.md").write_text(
        "\n".join(lines) + "\n", encoding="utf-8"
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("run_dirs", nargs="+", type=Path)
    parser.add_argument("--warmup-frames", type=int, default=0)
    args = parser.parse_args()
    if args.warmup_frames < 0:
        parser.error("--warmup-frames must be non-negative")

    summaries = [analyze_run(path, args.warmup_frames) for path in args.run_dirs]
    if len(summaries) > 1:
        parents = {Path(summary["run_dir"]).resolve().parent for summary in summaries}
        if len(parents) != 1:
            raise ValueError("All run directories must have the same parent")
        common_parent = parents.pop()
        write_aggregate(common_parent, summaries)
        print(common_parent / "aggregate_summary.md")
    else:
        print(args.run_dirs[0] / "summary.md")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
