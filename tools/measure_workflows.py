#!/usr/bin/env python3
"""Measure real Editor/import/C++ iteration on a disposable copy of a sample project.

Numbers are observations, not CI pass/fail thresholds. Linux peak RSS comes from
GNU time for the command and its waited-for children; other platforms report null.
"""
import argparse
import datetime
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--editor", type=Path, required=True)
    parser.add_argument("--project", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    editor, source, output = args.editor.resolve(), args.project.resolve(), args.output.resolve()
    if output.exists():
        raise RuntimeError("Use a new output directory; existing evidence is never overwritten")
    output.mkdir(parents=True)
    project = output / "project"
    shutil.copytree(source, project, ignore=shutil.ignore_patterns(".faset", "Exports", "*.blend1"))
    samples = []

    def run(label, arguments, timeout=1800):
        memory = output / (label + "-memory.json")
        command = [str(arg) for arg in arguments]
        wrapped = command
        if platform.system() == "Linux" and Path("/usr/bin/time").is_file():
            wrapped = ["/usr/bin/time", "-f", '{"peak_rss_kib":%M}', "-o", str(memory), *command]
        started = time.perf_counter()
        result = subprocess.run(wrapped, cwd=output, capture_output=True, text=True,
                                encoding="utf-8", timeout=timeout)
        elapsed = time.perf_counter() - started
        (output / (label + "-stdout.txt")).write_text(result.stdout, encoding="utf-8")
        (output / (label + "-stderr.txt")).write_text(result.stderr, encoding="utf-8")
        sample = {"name": label, "seconds": elapsed, "returncode": result.returncode,
                  "peak_rss_kib": json.loads(memory.read_text())["peak_rss_kib"] if memory.exists() else None}
        samples.append(sample)
        (output / "progress.json").write_text(json.dumps(samples, indent=2), encoding="utf-8")
        if result.returncode:
            raise RuntimeError(f"{label} failed: {result.stderr[-4000:]} {result.stdout[-2000:]}")
        print(f"{label}: {elapsed:.3f}s, peak RSS {sample['peak_rss_kib']} KiB", flush=True)
        return result

    def call(label, name, arguments=None):
        result = run(label, [editor, "--project", project, "--command",
                            json.dumps({"name": name, "arguments": arguments or {}}), "--wait"])
        document = json.loads(result.stdout)
        if "state" in document and document["state"] != "succeeded":
            raise RuntimeError(f"{label} job did not succeed: {document}")
        return document

    for index in range(3):
        call(f"headless-startup-{index + 1}", "faset_project")
    # Separate GUI startup from headless command latency. Includes two frames and
    # shutdown, not a claimed first-visible-frame timestamp.
    run("editor-gui-two-frames", [editor, "--project", project, "--frames", "2"])
    manifest = project / "Assets/exit-arch/manifest.json"
    if manifest.exists():
        call("import-empty-cache", "faset_import", {"path": "Assets/exit-arch/manifest.json"})
        call("import-existing-generation", "faset_import", {"path": "Assets/exit-arch/manifest.json"})
    call("initial-debug-build", "faset_build")
    call("unchanged-debug-build", "faset_build")
    gameplay = project / "Scripts/Gameplay.cpp"
    original = gameplay.read_bytes()
    gameplay.write_bytes(original + b"\n// Measurement: one gameplay translation unit changed.\n")
    # Verify the user-facing stale status before a successful replacement.
    assert call("schema-stale-after-edit", "faset_schema_status")["stale"]
    iteration_started = time.perf_counter()
    built = call("changed-debug-build", "faset_build")["result"]
    profile = output / "development-player.json"
    settings = json.loads((project / "project.faset.json").read_text(encoding="utf-8"))
    run("changed-build-player-first-frame", [built["player"], "--scene",
        project / settings["start_scene"], "--assets", project / ".faset/cache",
        "--headless", "--frames", "1", "--profile", profile])
    iteration_seconds = time.perf_counter() - iteration_started
    assert not call("schema-current-after-build", "faset_schema_status")["stale"]
    # Keep original inputs in this disposable project, without implying its last
    # built binary matches the restored source signature.
    gameplay.write_bytes(original)
    engine = Path(__file__).resolve().parents[1]
    revision = subprocess.run(["git", "rev-parse", "HEAD"], cwd=engine,
                              capture_output=True, text=True).stdout.strip()
    dirty = subprocess.run(["git", "status", "--porcelain"], cwd=engine,
                           capture_output=True, text=True).stdout.strip()
    cpu = platform.processor()
    if Path("/proc/cpuinfo").exists():
        cpu = next((line.split(":", 1)[1].strip() for line in Path("/proc/cpuinfo").read_text().splitlines()
                    if line.startswith("model name")), cpu)
    report = {
        "format": "faset.workflow-measurements", "version": 1,
        "recorded_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "revision": revision, "working_tree_dirty": bool(dirty),
        "host": {"platform": platform.platform(), "cpu": cpu,
                 "logical_cpus": os.cpu_count(), "python": platform.python_version()},
        "source_project": str(source), "editor": str(editor),
        "build_configuration": "Debug", "samples": samples,
        "changed_build_and_one_frame_process_seconds": iteration_seconds,
        "player": json.loads(profile.read_text()),
        "method": [
            "Fresh disposable project, engine dependency archives and OS file caches already available.",
            "Each Editor command starts a new process and includes shutdown in wall time.",
            "GUI startup sample includes window creation, two frames, and shutdown.",
            "Changed build appends a harmless comment, recompiles Gameplay.cpp and relinks native outputs.",
            "Combined iteration runs the returned Debug Player in a separate process for one offscreen frame.",
            "Peak RSS is GNU time maximum over each command and waited-for children, not a sum.",
            "Numbers include ambient host load; there is no warm-up exclusion or real-time frame guarantee."
        ]}
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
