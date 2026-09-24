#!/usr/bin/env python3
"""Profile the two relocated Release samples retained by verify_playable_exports.py."""

import argparse
import hashlib
import json
from pathlib import Path
import platform
import shutil
import subprocess
import time


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def write(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def run_source_hidden(source: dict, command: list[str], cwd: Path) -> subprocess.CompletedProcess:
    projects = Path(source["project_workspace"]) / "Faset Café 世界"
    disabled = projects.parent / "projects-offline-reference"
    require(projects.is_dir() and not disabled.exists(),
            "Disposable source projects must be available to hide")
    projects.rename(disabled)
    try:
        require(not projects.exists(), "Source project paths remain available")
        return subprocess.run(command, cwd=cwd, capture_output=True, text=True,
                              encoding="utf-8", errors="replace", timeout=300)
    finally:
        if disabled.exists() and not projects.exists():
            disabled.rename(projects)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verification", type=Path, required=True,
                        help="Passed report.json from tools/verify_playable_exports.py")
    parser.add_argument("--output", type=Path, required=True,
                        help="New/empty directory for raw 240-frame evidence")
    args = parser.parse_args()
    verification = args.verification.resolve()
    output = args.output.resolve()
    source = read(verification)
    require(source.get("status") == "passed" and not source.get("engine_dirty"),
            "A clean, passed export verification is required")
    require(source.get("frames_per_game") == 120, "Expected a 120-frame verifier")
    require(set(source.get("requested_projects", [])) ==
            {"collect-2d", "collect-3d", "lua"} and
            {item.get("name") for item in source.get("projects", [])} ==
            {"collect-2d", "collect-3d", "lua"} and
            all(item.get("status") == "passed" and
                item.get("completed_frames") == 120 and
                item.get("source_project_paths_unavailable") is True
                for item in source["projects"]),
            "Expected three source-hidden 120-frame Release games")
    require(not output.exists() or not any(output.iterdir()),
            "Output directory must be new or empty")
    output.mkdir(parents=True, exist_ok=True)
    shutil.copy2(verification, output / "playable-report.json")
    cwd = Path(source["standalone_root"]) / "empty-working-directory"
    require(cwd.is_dir(), "Verifier's unrelated working directory is missing")
    limits = {"wall": 4.0, "gpu": 1.0, "renderer_readback_cpu": 1.0,
              "simulation": 0.5, "snapshot": 0.5}
    report = {"format": "faset.p1-release-reference", "version": 1,
              "source_revision": source["engine_revision"],
              "editor_sha256": source["editor_sha256"],
              "verification_sha256": digest(verification),
              "host": {"platform": platform.platform(), "machine": platform.machine(),
                       "processor": platform.processor()},
              "method": "sequential relocated Release Player, default direct/temporal-off, "
                        "offscreen 1280x720, 240 completed frames including first; "
                        "nearest-rank p95, validation/readback enabled",
              "projects": []}
    for item in source["projects"]:
        if item["name"] not in ("collect-2d", "collect-3d"):
            continue
        require(item.get("status") == "passed" and item.get("configuration") == "Release" and
                item.get("source_project_paths_unavailable") is True,
                f"Incomplete relocated export: {item['name']}")
        name = item["name"]
        package = Path(item["standalone_directory"])
        player = package / item["executable"]
        require(player.is_file() and not player.is_symlink(), f"Missing Player: {player}")
        profile_path = output / f"{name}-profile-240.json"
        command = [str(player), "--headless", "--frames", "240", "--profile",
                   str(profile_path)]
        started = time.monotonic()
        process = run_source_hidden(source, command, cwd)
        invocation = {"arguments": command, "cwd": str(cwd),
                      "source_project_paths_unavailable": True,
                      "seconds": time.monotonic() - started,
                      "exit_code": process.returncode,
                      "stdout": process.stdout, "stderr": process.stderr}
        write(output / f"{name}-run-240.json", invocation)
        require(process.returncode == 0 and profile_path.is_file(),
                f"240-frame Player failed; inspect {name}-run-240.json")
        profile = read(profile_path)
        require(profile.get("format") == "faset.player-profile" and
                profile.get("completed_frames") == 240 and
                len(profile.get("samples", [])) == 240 and
                profile.get("dimension") == item["dimension"] and
                profile.get("presentation_mode") == "offscreen" and
                profile.get("width") == 1280 and profile.get("height") == 720 and
                profile.get("visibility_mode") == "direct" and
                profile.get("effective_visibility_mode") == "direct" and
                profile.get("temporal_mode") == "off" and
                profile.get("effective_temporal_mode") == "off" and
                profile.get("percentile_method") ==
                "nearest_rank_all_completed_frames_no_warmup_exclusion" and
                profile.get("validation_enabled") is True and
                profile.get("validation_errors") == 0,
                f"Invalid reference profile: {profile_path}")
        p95 = {key: profile["summary_ms"][key]["p95"] for key in limits}
        allocated = max(sample["gpu_allocated_bytes"] for sample in profile["samples"])
        sun_atlas = max(sample["sun_shadow_atlas_bytes"] for sample in profile["samples"])
        local_atlas = max(sample["local_shadow_atlas_bytes"] for sample in profile["samples"])
        startup = profile["startup_ms"]["main_to_first_frame"]
        checks = {key: {"observed_ms": value, "limit_ms": limits[key],
                        "met": value is not None and value <= limits[key]}
                  for key, value in p95.items()}
        checks["explicit_allocation"] = {"observed_bytes": allocated,
                                           "limit_bytes": 20 * 1024 * 1024,
                                           "met": allocated <= 20 * 1024 * 1024}
        checks["startup"] = {"observed_ms": startup, "limit_ms": 500.0,
                             "met": startup <= 500.0}
        result = {"name": name, "dimension": item["dimension"],
                  "device": profile["device"], "player_sha256": digest(player),
                  "manifest_sha256": digest(package / "manifest.json"),
                  "profile_sha256": digest(profile_path),
                  "requested_frames": profile["requested_frames"],
                  "completed_frames": profile["completed_frames"],
                  "summary_ms": profile["summary_ms"], "startup_ms": profile["startup_ms"],
                  "effective_lighting_path": profile.get("effective_lighting_path"),
                  "gpu_allocated_bytes_max": allocated,
                  "sun_shadow_atlas_bytes_max": sun_atlas,
                  "local_shadow_atlas_bytes_max": local_atlas,
                  "gpu_allocated_excluding_shadow_atlases_bytes_max": max(
                      sample["gpu_allocated_bytes"] -
                      sample["sun_shadow_atlas_bytes"] -
                      sample["local_shadow_atlas_bytes"]
                      for sample in profile["samples"]),
                  "effective_sun_cascades_max": max(
                      sample["effective_sun_cascades"] for sample in profile["samples"]),
                  "local_shadow_faces_max": max(
                      sample["local_shadow_faces"] for sample in profile["samples"]),
                  "budget_checks": checks}
        report["projects"].append(result)
        write(output / "reference-summary.json", report)
    require({item["name"] for item in report["projects"]} == {"collect-2d", "collect-3d"},
            "Both exact sample scenes are required")
    print(output / "reference-summary.json")


if __name__ == "__main__":
    main()
