#!/usr/bin/env python3
"""Run the fixed P3 lighting sweep and retain raw per-frame GPU measurements.

The Forward+ threshold in the summary is a measurement result, not an automatic
renderer switch. Apply it to the Linux physical reference GPU; keep other devices
as separate functional/performance observations.
"""
from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path
import statistics
import subprocess
import sys


LIGHT_COUNTS = (0, 4, 16, 32, 64, 128)
VISIBILITY_MODES = ("direct", "gpu-frustum", "gpu-occlusion")
REPEATS = (1, 2, 3)
WARMUP_FRAMES = 10
MEASURED_FRAMES = 30
WIDTH, HEIGHT = 1920, 1080
REQUIRED_COLUMNS = (
    "light_count", "shadows", "visibility", "frame", "device", "driver",
    "commit", "gpu_main_raster_ms", "gpu_ms", "gpu_shadow_ms", "cpu_ms",
    "readback_cpu_ms", "validation_errors", "run_index", "effective_visibility",
    "lighting_path", "submitted_local_lights", "omitted_local_lights",
    "shadow_tiles", "draw_calls", "gpu_bytes", "validation_enabled", "width", "height",
)
TIMING_COLUMNS = ("gpu_main_raster_ms", "gpu_ms", "gpu_shadow_ms", "cpu_ms",
                  "readback_cpu_ms")


def build_runs(shadows: str = "both") -> list[dict]:
    if shadows not in ("off", "on", "both"):
        raise ValueError(f"Unsupported shadow setting: {shadows}")
    settings = ("off", "on") if shadows == "both" else (shadows,)
    return [{"shadows": shadow, "visibility": mode, "light_count": lights,
             "repeat": repeat}
            for shadow in settings for mode in VISIBILITY_MODES
            for lights in LIGHT_COUNTS for repeat in REPEATS]


def median(values: list[float]) -> float:
    if not values:
        raise ValueError("Cannot summarize empty measurements")
    return float(statistics.median(values))


def p95(values: list[float]) -> float:
    if not values:
        raise ValueError("Cannot summarize empty measurements")
    ordered = sorted(values)
    return ordered[math.ceil(.95 * len(ordered)) - 1]


def _measurement(row: dict, name: str) -> float:
    try:
        value = float(row[name])
    except (KeyError, TypeError, ValueError) as error:
        raise ValueError(f"Invalid {name} in benchmark CSV") from error
    if not math.isfinite(value) or value < 0:
        raise ValueError(f"Invalid {name} in benchmark CSV: {value}")
    return value


def summarize_rows(rows: list[dict]) -> dict:
    """Use the median of each independent run's median, then compare like baselines."""
    grouped: dict[tuple[str, str, int], dict[int, list[dict]]] = {}
    for row in rows:
        try:
            key = (row["shadows"], row["visibility"], int(row["light_count"]))
            repeat = int(row["run_index"])
        except (KeyError, TypeError, ValueError) as error:
            raise ValueError("Benchmark row lacks shadow/mode/light/repeat identity") from error
        if key[0] not in ("off", "on") or key[1] not in VISIBILITY_MODES or repeat < 1:
            raise ValueError(f"Invalid benchmark configuration: {key}, repeat {repeat}")
        for name in TIMING_COLUMNS:
            _measurement(row, name)
        grouped.setdefault(key, {}).setdefault(repeat, []).append(row)

    configurations = []
    lookup = {}
    for (shadows, visibility, lights), repeats in sorted(grouped.items()):
        run_summaries = []
        for repeat, samples in sorted(repeats.items()):
            run_summaries.append({
                "run_index": repeat, "frames": len(samples),
                "median_ms": {name: median([_measurement(row, name) for row in samples])
                              for name in TIMING_COLUMNS},
            })
        entry = {
            "shadows": shadows, "visibility": visibility, "light_count": lights,
            "runs": run_summaries,
            "median_ms": {name: median([run["median_ms"][name] for run in run_summaries])
                          for name in TIMING_COLUMNS},
            "p95_ms": {name: p95([_measurement(row, name) for samples in repeats.values()
                                   for row in samples]) for name in TIMING_COLUMNS},
        }
        configurations.append(entry)
        lookup[(shadows, visibility, lights)] = entry

    candidates = []
    evaluated = []
    for entry in configurations:
        if entry["light_count"] not in (32, 64, 128):
            continue
        baseline = lookup.get((entry["shadows"], entry["visibility"], 0))
        if baseline is None:
            raise ValueError("Forward+ gate requires a zero-light baseline for each mode/shadow setting")
        zero_gpu = baseline["median_ms"]["gpu_ms"]
        if zero_gpu <= 0:
            raise ValueError("Forward+ gate requires positive zero-light GPU frame timing")
        overhead = (entry["median_ms"]["gpu_main_raster_ms"] -
                    baseline["median_ms"]["gpu_main_raster_ms"])
        result = {"shadows": entry["shadows"], "visibility": entry["visibility"],
                  "light_count": entry["light_count"], "overhead_ms": overhead,
                  "zero_light_gpu_ms": zero_gpu,
                  "overhead_percent_of_zero_gpu": 100 * overhead / zero_gpu,
                  "absolute_threshold_reached": overhead >= 1.0,
                  "relative_threshold_reached": overhead >= .15 * zero_gpu}
        evaluated.append(result)
        if result["absolute_threshold_reached"] or result["relative_threshold_reached"]:
            candidates.append(result)
    return {"configurations": configurations,
            "forward_plus_gate": {"triggered": bool(candidates), "candidates": candidates,
                                  "evaluated": evaluated,
                                  "basis": "median of three independent run medians; same-mode/shadow zero-light GPU baseline"}}


def _read_run_csv(path: Path, run: dict, commit: str) -> tuple[list[str], list[dict]]:
    with path.open(newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream)
        columns = reader.fieldnames or []
        missing = sorted(set(REQUIRED_COLUMNS) - set(columns))
        if missing:
            raise ValueError(f"{path}: missing CSV column(s): {', '.join(missing)}")
        rows = list(reader)
    if len(rows) != MEASURED_FRAMES:
        raise ValueError(f"{path}: expected {MEASURED_FRAMES} measured frames, got {len(rows)}")
    frames = set()
    for row in rows:
        expected = {"light_count": str(run["light_count"]), "shadows": run["shadows"],
                    "visibility": run["visibility"], "run_index": str(run["repeat"]),
                    "commit": commit, "width": str(WIDTH), "height": str(HEIGHT)}
        for name, value in expected.items():
            if row[name] != value:
                raise ValueError(f"{path}: {name} mismatch: expected {value}, got {row[name]}")
        if row["effective_visibility"] != run["visibility"]:
            raise ValueError(f"{path}: effective_visibility fell back from {run['visibility']}")
        if row["submitted_local_lights"] != str(run["light_count"]) or row["omitted_local_lights"] != "0":
            raise ValueError(f"{path}: submitted_local_lights or omitted_local_lights disagrees with the workload")
        try:
            frame = int(row["frame"])
            errors = int(row["validation_errors"])
        except ValueError as error:
            raise ValueError(f"{path}: invalid frame or validation error count") from error
        if frame in frames or errors != 0:
            raise ValueError(f"{path}: duplicate frame or Vulkan validation error")
        frames.add(frame)
        if not row["device"] or not row["driver"] or not row["lighting_path"]:
            raise ValueError(f"{path}: device, driver and effective lighting path are required")
        for name in TIMING_COLUMNS:
            _measurement(row, name)
    return columns, rows


def _git_revision() -> str:
    root = Path(__file__).resolve().parents[1]
    return subprocess.check_output(["git", "-C", str(root), "rev-parse", "HEAD"],
                                   text=True).strip()


def sweep(executable: Path, output: Path, shadows: str, commit: str,
          validation: str = "off", driver: str | None = None) -> dict:
    if not executable.is_file():
        raise ValueError(f"Benchmark executable does not exist: {executable}")
    if output.exists() and any(output.iterdir()):
        raise ValueError(f"Output directory must be new or empty: {output}")
    raw = output / "raw"
    raw.mkdir(parents=True)
    all_rows = []
    columns = None
    runs = build_runs(shadows)
    command_prefix = [sys.executable, str(executable)] if executable.suffix.lower() == ".py" else [str(executable)]
    for run in runs:
        filename = (f"shadows-{run['shadows']}_{run['visibility']}_"
                    f"lights-{run['light_count']:03d}_run-{run['repeat']}.csv")
        target = raw / filename
        command = command_prefix + [
            "--lights", str(run["light_count"]), "--shadows", run["shadows"],
            "--visibility", run["visibility"], "--csv", str(target),
            "--run-index", str(run["repeat"]), "--commit", commit,
            "--validation", validation, "--width", str(WIDTH), "--height", str(HEIGHT),
            "--warmup", str(WARMUP_FRAMES), "--frames", str(MEASURED_FRAMES),
        ]
        if driver is not None:
            command += ["--driver", driver]
        result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8",
                                errors="replace")
        if result.returncode != 0:
            raise RuntimeError(f"Benchmark failed for {filename}: {result.stderr[-2000:]}")
        run_columns, samples = _read_run_csv(target, run, commit)
        if columns is None:
            columns = run_columns
        elif columns != run_columns:
            raise ValueError(f"{target}: CSV schema differs from other runs")
        all_rows.extend({**row, "source_csv": filename} for row in samples)

    merged = output / "merged.csv"
    with merged.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=[*(columns or []), "source_csv"])
        writer.writeheader()
        writer.writerows(all_rows)
    summary = {"format": "faset.p3-lighting-benchmark", "version": 1,
               "commit": commit, "warmup_frames_per_run": WARMUP_FRAMES,
               "measured_frames_per_run": MEASURED_FRAMES, "width": WIDTH, "height": HEIGHT,
               "validation": validation, "runs_completed": len(runs), "rows": len(all_rows),
               **summarize_rows(all_rows)}
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n",
                                          encoding="utf-8")
    return summary


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--list-runs", action="store_true", help="Print the deterministic sweep matrix as JSON")
    mode.add_argument("--sweep", action="store_true", help="Run every configuration and retain raw CSV")
    parser.add_argument("--shadows", choices=("off", "on", "both"), default="both")
    parser.add_argument("--executable", type=Path, help="Built C++ benchmark executable")
    parser.add_argument("--output", type=Path, help="New or empty evidence directory")
    parser.add_argument("--commit", help="Source revision; defaults to this checkout's HEAD")
    parser.add_argument("--driver", help="Explicit driver label to pass through to the benchmark")
    parser.add_argument("--validation", choices=("on", "off"), default="off")
    args = parser.parse_args()
    if args.list_runs:
        print(json.dumps({"format": "faset.p3-lighting-run-matrix", "version": 1,
                          "runs": build_runs(args.shadows)}, indent=2))
        return 0
    if args.executable is None or args.output is None:
        parser.error("--sweep requires --executable and --output")
    try:
        summary = sweep(args.executable.resolve(), args.output.resolve(), args.shadows,
                        args.commit or _git_revision(), args.validation, args.driver)
    except (OSError, ValueError, RuntimeError) as error:
        print(f"P3 lighting benchmark failed: {error}", file=sys.stderr)
        return 1
    print(json.dumps({"summary": str(args.output.resolve() / "summary.json"),
                      "runs_completed": summary["runs_completed"],
                      "forward_plus_threshold_reached": summary["forward_plus_gate"]["triggered"]}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
