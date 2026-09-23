#!/usr/bin/env python3
"""Run the fixed P3 lighting sweep and retain raw per-frame GPU measurements.

The Forward+ threshold in the summary is a measurement result, not an automatic
renderer switch. Apply it to the Linux physical reference GPU; keep other devices
as separate functional/performance observations.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import statistics
import subprocess
import sys


LIGHT_COUNTS = (0, 4, 16, 32, 64, 128)
VISIBILITY_MODES = ("direct", "gpu-frustum", "gpu-occlusion")
REPEATS = (1, 2, 3)
# Pair a zero-light control with every independent repeat, keeping high-light
# measurements nearby rather than comparing the first run with the last.
MEASUREMENT_ORDER = (0, 32, 4, 64, 16, 128)
WARMUP_FRAMES = 10
MEASURED_FRAMES = 30
WIDTH, HEIGHT = 1920, 1080
REQUIRED_COLUMNS = (
    "light_count", "shadows", "visibility", "frame", "device", "driver",
    "commit", "gpu_main_raster_ms", "gpu_ms", "gpu_shadow_ms", "cpu_ms",
    "readback_cpu_ms", "validation_errors", "run_index", "effective_visibility",
    "lighting_path", "submitted_local_lights", "omitted_local_lights",
    "shadow_tiles", "draw_calls", "gpu_bytes", "validation_enabled", "width", "height",
    "build_configuration", "requested_local_shadow_faces", "rendered_local_shadow_faces",
    "dropped_shadow_faces", "shadow_atlas_full_drops", "gpu_post_raster_ms",
    "gpu_post_visible",
)
TIMING_COLUMNS = ("gpu_main_raster_ms", "gpu_post_raster_ms", "gpu_ms",
                  "gpu_shadow_ms", "cpu_ms", "readback_cpu_ms")
SUMMARY_TIMINGS = (*TIMING_COLUMNS, "forward_raster_ms")
SHADOW_COLUMNS = ("requested_local_shadow_faces", "rendered_local_shadow_faces",
                  "shadow_tiles", "dropped_shadow_faces", "shadow_atlas_full_drops")


def build_runs(shadows: str = "both") -> list[dict]:
    if shadows not in ("off", "on", "both"):
        raise ValueError(f"Unsupported shadow setting: {shadows}")
    settings = ("off", "on") if shadows == "both" else (shadows,)
    return [{"shadows": shadow, "visibility": mode, "light_count": lights,
             "repeat": repeat}
            for shadow in settings for repeat in REPEATS
            for mode in VISIBILITY_MODES for lights in MEASUREMENT_ORDER]


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


def _timing(row: dict, name: str) -> float:
    if name == "forward_raster_ms":
        main = _measurement(row, "gpu_main_raster_ms")
        return main + (_measurement(row, "gpu_post_raster_ms")
                       if row["visibility"] == "gpu-occlusion" else 0)
    return _measurement(row, name)


def _count(row: dict, name: str) -> int:
    try:
        value = int(row[name])
    except (KeyError, TypeError, ValueError) as error:
        raise ValueError(f"Invalid {name} in benchmark CSV") from error
    if value < 0:
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
        for name in SHADOW_COLUMNS:
            _count(row, name)
        grouped.setdefault(key, {}).setdefault(repeat, []).append(row)

    configurations = []
    lookup = {}
    for (shadows, visibility, lights), repeats in sorted(grouped.items()):
        run_summaries = []
        for repeat, samples in sorted(repeats.items()):
            run_summaries.append({
                "run_index": repeat, "frames": len(samples),
                "median_ms": {name: median([_timing(row, name) for row in samples])
                              for name in SUMMARY_TIMINGS},
            })
        shadow_counts = {name: {_count(row, name) for samples in repeats.values()
                                for row in samples} for name in SHADOW_COLUMNS}
        if any(len(values) != 1 for values in shadow_counts.values()):
            raise ValueError("Shadow counts changed within a fixed benchmark configuration")
        entry = {
            "shadows": shadows, "visibility": visibility, "light_count": lights,
            "runs": run_summaries,
            "shadow_counts": {name: next(iter(values)) for name, values in shadow_counts.items()},
            "median_ms": {name: median([run["median_ms"][name] for run in run_summaries])
                          for name in SUMMARY_TIMINGS},
            "p95_ms": {name: p95([_timing(row, name) for samples in repeats.values()
                                   for row in samples]) for name in SUMMARY_TIMINGS},
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
        base_runs = {run["run_index"]: run for run in baseline["runs"]}
        paired = [(run, base_runs[run["run_index"]]) for run in entry["runs"]
                  if run["run_index"] in base_runs]
        if len(paired) != len(entry["runs"]):
            raise ValueError("Forward+ gate requires a matched zero-light repeat for every run")
        zero_gpu = median([base["median_ms"]["gpu_ms"] for _, base in paired])
        if zero_gpu <= 0:
            raise ValueError("Forward+ gate requires positive zero-light GPU frame timing")
        overhead = median([run["median_ms"]["forward_raster_ms"] -
                           base["median_ms"]["forward_raster_ms"] for run, base in paired])
        result = {"shadows": entry["shadows"], "visibility": entry["visibility"],
                  "light_count": entry["light_count"], "overhead_ms": overhead,
                  "raster_metric": ("main_plus_post" if entry["visibility"] == "gpu-occlusion"
                                    else "main"),
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
                                  "basis": "median of paired per-run raster overheads; same-mode/shadow/repeat zero-light GPU baseline"}}


def _read_run_csv(path: Path, run: dict, commit: str, driver: str,
                  validation: str) -> tuple[list[str], list[dict]]:
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
        if row["build_configuration"] != "Release":
            raise ValueError(f"{path}: Forward+ sweep requires a Release benchmark binary")
        if row["driver"] != driver:
            raise ValueError(f"{path}: driver differs from the declared driver identity")
        if row["validation_enabled"] != ("1" if validation == "on" else "0"):
            raise ValueError(f"{path}: validation_enabled differs from the requested setting")
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
        requested = _count(row, "requested_local_shadow_faces")
        rendered = _count(row, "rendered_local_shadow_faces")
        tiles = _count(row, "shadow_tiles")
        dropped = _count(row, "dropped_shadow_faces")
        atlas_drops = _count(row, "shadow_atlas_full_drops")
        expected_faces = 6 * run["light_count"] if run["shadows"] == "on" else 0
        if (requested != expected_faces or rendered > tiles or tiles > min(requested, 16) or
                dropped > requested or atlas_drops > dropped):
            raise ValueError(f"{path}: requested/effective shadow face counts disagree with the workload")
        _count(row, "gpu_post_visible")
        for name in TIMING_COLUMNS:
            _measurement(row, name)
    return columns, rows


def _git_revision() -> str:
    root = Path(__file__).resolve().parents[1]
    return subprocess.check_output(["git", "-C", str(root), "rev-parse", "HEAD"],
                                   text=True).strip()


def _binary_sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def _clean_source_revision(root: Path, expected: str) -> str:
    revision = subprocess.check_output(["git", "-C", str(root), "rev-parse", "HEAD"],
                                       text=True).strip()
    if revision != expected:
        raise ValueError(f"Source revision {revision} disagrees with benchmark commit {expected}")
    status = subprocess.check_output(["git", "-C", str(root), "status", "--porcelain",
                                      "--untracked-files=all"], text=True)
    if status:
        raise ValueError("Benchmark source checkout is dirty; commit sources before measurement")
    return revision


def sweep(executable: Path, output: Path, shadows: str, commit: str,
          validation: str = "off", driver: str | None = None,
          source_root: Path | None = None) -> dict:
    if not executable.is_file():
        raise ValueError(f"Benchmark executable does not exist: {executable}")
    if driver is None or not driver.strip() or driver.strip().lower() == "unknown":
        raise ValueError("A measured sweep requires an explicit --driver identity")
    source = (source_root or Path(__file__).resolve().parents[1]).resolve()
    revision = _clean_source_revision(source, commit)
    binary_sha256 = _binary_sha256(executable)
    if output.exists() and any(output.iterdir()):
        raise ValueError(f"Output directory must be new or empty: {output}")
    raw = output / "raw"
    raw.mkdir(parents=True)
    all_rows = []
    columns = None
    identity = None
    runs = build_runs(shadows)
    command_prefix = [sys.executable, str(executable)] if executable.suffix.lower() == ".py" else [str(executable)]
    run_order = []
    for acquisition_index, run in enumerate(runs):
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
                                errors="replace", timeout=180)
        if result.returncode != 0:
            raise RuntimeError(f"Benchmark failed for {filename}: {result.stderr[-2000:]}")
        run_columns, samples = _read_run_csv(target, run, commit, driver, validation)
        if columns is None:
            columns = run_columns
        elif columns != run_columns:
            raise ValueError(f"{target}: CSV schema differs from other runs")
        for row in samples:
            actual = (row["device"], row["driver"], row["lighting_path"],
                      row["validation_enabled"], row["build_configuration"])
            if identity is None:
                identity = actual
            elif identity != actual:
                changed = next(name for name, before, after in zip(
                    ("device", "driver", "lighting_path", "validation_enabled",
                     "build_configuration"), identity, actual) if before != after)
                raise ValueError(f"{target}: {changed} changed during the sweep")
            all_rows.append({**row, "source_csv": filename,
                             "acquisition_index": acquisition_index})
        run_order.append(filename)

    merged = output / "merged.csv"
    with merged.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=[*(columns or []), "source_csv",
                                                    "acquisition_index"])
        writer.writeheader()
        writer.writerows(all_rows)
    if _binary_sha256(executable) != binary_sha256 or _clean_source_revision(source, commit) != revision:
        raise ValueError("Benchmark binary or source changed during the sweep")
    summary = {"format": "faset.p3-lighting-benchmark", "version": 1,
               "commit": commit, "warmup_frames_per_run": WARMUP_FRAMES,
               "measured_frames_per_run": MEASURED_FRAMES, "width": WIDTH, "height": HEIGHT,
               "validation": validation, "driver": driver,
               "source_revision": revision, "source_root": str(source), "source_dirty": False,
               "benchmark_sha256": binary_sha256,
               "device": identity[0], "lighting_path": identity[2],
               "validation_enabled": identity[3] == "1", "build_configuration": identity[4],
               "run_order": run_order,
               "runs_completed": len(runs), "rows": len(all_rows),
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
    parser.add_argument("--source-root", type=Path,
                        help="Clean source checkout used to build the benchmark; defaults to this repo")
    parser.add_argument("--driver", help="Required driver identity for a measured sweep")
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
                        args.commit or _git_revision(), args.validation, args.driver,
                        args.source_root)
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"P3 lighting benchmark failed: {error}", file=sys.stderr)
        return 1
    print(json.dumps({"summary": str(args.output.resolve() / "summary.json"),
                      "runs_completed": summary["runs_completed"],
                      "forward_plus_threshold_reached": summary["forward_plus_gate"]["triggered"]}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
