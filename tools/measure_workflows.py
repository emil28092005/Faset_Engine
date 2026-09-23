#!/usr/bin/env python3
"""Measure real Editor/import/C++ iteration on a disposable copy of a sample project.

Numbers are observations, not CI pass/fail thresholds. Linux peak RSS comes from
GNU time for the command and its waited-for children; other platforms report null.
"""
import argparse
import copy
import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import queue
import random
import shutil
import statistics
import subprocess
import threading
import time


def summarize(values):
    """Median and nearest-rank p95 of positive, finite observations."""
    if not values or any(not isinstance(value, (int, float)) or
                         not math.isfinite(value) or value <= 0 for value in values):
        raise ValueError("A distribution requires positive finite samples")
    ordered = sorted(values)
    return {"median": statistics.median(ordered),
            "p95": ordered[math.ceil(.95 * len(ordered)) - 1]}


def parse_peak_rss(text):
    # GNU time prefixes its -o output with a status sentence when the measured
    # command exits nonzero. The JSON format is the final nonempty line.
    for line in reversed(text.splitlines()):
        try:
            value = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(value, dict) and isinstance(value.get("peak_rss_kib"), int):
            return value["peak_rss_kib"]
    raise ValueError("GNU time did not produce a peak RSS record")


def validate_report(report):
    if report.get("format") != "faset.workflow-measurements" or report.get("version") != 2:
        raise ValueError("Expected a version 2 Faset workflow report")
    if not isinstance(report.get("revision"), str) or not report["revision"].strip():
        raise ValueError("A source revision is required")
    hashes = report.get("source_hashes")
    if not isinstance(hashes, dict) or not hashes or any(
            not isinstance(value, str) or len(value) != 64 or
            any(character not in "0123456789abcdef" for character in value)
            for value in hashes.values()):
        raise ValueError("Exact input SHA-256 hashes are required")
    configuration = report.get("build_configuration")
    if configuration not in {"Debug", "Release", "RelWithDebInfo"}:
        raise ValueError("Build configuration is missing or unknown")
    samples = report.get("samples")
    if not isinstance(samples, dict) or not samples:
        raise ValueError("Raw labelled samples are required")
    for label, rows in samples.items():
        if not isinstance(rows, list) or not rows:
            raise ValueError(f"{label} has no raw samples")
        if (label.startswith("warm_") or label.startswith("changed_")) and len(rows) < 5:
            raise ValueError(f"{label} requires at least five measured repetitions")
        for row in rows:
            if row.get("build_configuration") != configuration:
                raise ValueError(f"{label} mixes build configurations")
            if row.get("source_hash") not in hashes.values():
                raise ValueError(f"{label} lacks a declared input source hash")
            summarize([row.get("seconds")])
            if label == "windowed_first_presented_frame" and \
                    row.get("presentation_mode") != "windowed":
                raise ValueError("An offscreen frame cannot be labelled windowed presentation")
            if label == "offscreen_first_rendered_frame" and \
                    row.get("presentation_mode") != "offscreen":
                raise ValueError("A windowed frame cannot be labelled offscreen")


def generate_scene_fixture(scene, *, seed, count):
    """Add deterministic visible objects without mutating the checked-in scene."""
    if not isinstance(seed, int) or count < 0 or not scene.get("entities"):
        raise ValueError("A seed, nonnegative count, and source entity are required")
    generated = copy.deepcopy(scene)
    rng = random.Random(seed)
    reference = next((entry for entry in scene["entities"] if any(
        component.get("type") in {"faset.mesh", "faset.sprite"}
        for component in entry.get("components", []))), scene["entities"][0])
    for index in range(count):
        entity = copy.deepcopy(reference)
        entity["id"] = f"measured-{seed}-{index:05d}"
        entity["name"] = f"Measured object {index + 1}"
        entity["parent"] = None
        entity["components"] = [component for component in entity.get("components", [])
                                if component.get("type") in {"faset.transform", "faset.mesh",
                                                              "faset.sprite"}]
        for component in entity["components"]:
            component["id"] = f"{entity['id']}-{component['type']}"
            fields = component.get("fields", {})
            if component["type"] == "faset.transform":
                position = list(fields.get("position", [0, 0, 0]))
                position[0] = ((index % 32) - 16) * .45 + rng.uniform(-.05, .05)
                position[2 if scene.get("dimension", 3) == 3 else 1] = \
                    (index // 32) * .45 + rng.uniform(-.05, .05)
                fields["position"] = position
                fields["scale"] = [.3, .3, .3]
            elif component["type"] == "faset.sprite":
                fields["size"] = [.3, .3]
        generated["entities"].append(entity)
    return generated


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--editor", type=Path, required=True)
    parser.add_argument("--project", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--lua-project", type=Path,
                        default=Path(__file__).resolve().parents[1] / "examples/lua")
    parser.add_argument("--ui-latency-binary", type=Path)
    args = parser.parse_args()
    editor, source, output = args.editor.resolve(), args.project.resolve(), args.output.resolve()
    lua_source = args.lua_project.resolve()
    ui_binary = (args.ui_latency_binary or
                 editor.parent / ("faset_editor_ui_latency.exe" if os.name == "nt" else
                                  "faset_editor_ui_latency")).resolve()
    for file in (editor, ui_binary, source / "project.faset.json",
                 lua_source / "project.faset.json"):
        if not file.is_file():
            raise RuntimeError(f"Required measurement input is missing: {file}")
    if output.exists():
        raise RuntimeError("Use a new output directory; existing evidence is never overwritten")
    output.mkdir(parents=True)
    project = output / "project"
    shutil.copytree(source, project, ignore=shutil.ignore_patterns(".faset", "Exports", "*.blend1"))
    lua_project = output / "lua-project"
    shutil.copytree(lua_source, lua_project,
                    ignore=shutil.ignore_patterns(".faset", "Exports", "*.blend1"))
    raw = output / "raw"
    raw.mkdir()
    samples = {}
    source_hashes = {}
    counters = {}

    def digest(path):
        return hashlib.sha256(path.read_bytes()).hexdigest()

    def remember(path, variant, base=project):
        key = f"{base.name}/{path.relative_to(base).as_posix()}#{variant}"
        value = digest(path)
        source_hashes[key] = value
        return value

    gameplay = project / "Scripts/Gameplay.cpp"
    header = project / "Scripts/Gameplay.hpp"
    if not gameplay.is_file() or not header.is_file():
        raise RuntimeError("The C++ project needs Scripts/Gameplay.cpp and Gameplay.hpp")
    baseline_hash = remember(gameplay, "baseline")
    project_hashes = {file.relative_to(project).as_posix(): digest(file)
                      for file in sorted(project.rglob("*")) if file.is_file()}
    lua_input = json.loads((lua_project / "project.faset.json").read_text(encoding="utf-8"))
    lua_file = lua_project / lua_input["scripting"]["lua"]["scripts"][0]
    lua_baseline_hash = remember(lua_file, "baseline", lua_project)

    def run(label, arguments, *, source_hash=baseline_hash, timeout=1800,
            expected_failure=False, record=True):
        index = counters.get(label, 0) + 1
        counters[label] = index
        stem = f"{label}-{index:02d}"
        memory = raw / (stem + "-memory.json")
        command = [str(arg) for arg in arguments]
        wrapped = command
        if platform.system() == "Linux" and Path("/usr/bin/time").is_file():
            wrapped = ["/usr/bin/time", "-f", '{"peak_rss_kib":%M}', "-o", str(memory), *command]
        started = time.perf_counter()
        result = subprocess.run(wrapped, cwd=output, capture_output=True, text=True,
                                encoding="utf-8", timeout=timeout)
        elapsed = time.perf_counter() - started
        stdout_file, stderr_file = raw / (stem + "-stdout.txt"), raw / (stem + "-stderr.txt")
        stdout_file.write_text(result.stdout, encoding="utf-8")
        stderr_file.write_text(result.stderr, encoding="utf-8")
        sample = {"seconds": elapsed, "returncode": result.returncode,
                  "build_configuration": "Debug", "source_hash": source_hash,
                  "command": command, "stdout": str(stdout_file.relative_to(output)),
                  "stderr": str(stderr_file.relative_to(output)),
                  "peak_rss_kib": parse_peak_rss(memory.read_text())
                  if memory.exists() else None}
        (raw / (stem + ".json")).write_text(json.dumps(sample, indent=2) + "\n",
                                              encoding="utf-8")
        if record:
            samples.setdefault(label, []).append(sample)
        (output / "progress.json").write_text(json.dumps(samples, indent=2) + "\n",
                                               encoding="utf-8")
        if expected_failure and result.returncode == 0:
            raise RuntimeError(f"{label} unexpectedly succeeded")
        if result.returncode and not expected_failure:
            raise RuntimeError(f"{label} failed: {result.stderr[-4000:]} {result.stdout[-2000:]}")
        print(f"{stem}: {elapsed:.3f}s, peak RSS {sample['peak_rss_kib']} KiB", flush=True)
        return result, sample

    def call(label, name, arguments=None, *, root=project, source_hash=baseline_hash,
             record=True):
        result, sample = run(label, [editor, "--project", root, "--command",
                                     json.dumps({"name": name, "arguments": arguments or {}}),
                                     "--wait"], source_hash=source_hash, record=record)
        document = json.loads(result.stdout)
        if "state" in document and document["state"] != "succeeded":
            raise RuntimeError(f"{label} job did not succeed: {document}")
        if name == "faset_build":
            sample["schema_cache_hit"] = document.get("result", {}).get("schema_cache_hit")
            sample["generation_reused"] = document.get("result", {}).get("generation_reused")
        return document, sample

    for index in range(3):
        call("headless_editor_command_startup", "faset_project")
    # This is window creation plus two frames and shutdown, not first-present latency.
    run("windowed_editor_two_frames", [editor, "--project", project, "--frames", "2"])
    manifest = project / "Assets/exit-arch/manifest.json"
    if manifest.exists():
        call("cold_asset_import", "faset_import", {"path": "Assets/exit-arch/manifest.json"})
        call("cached_asset_import", "faset_import", {"path": "Assets/exit-arch/manifest.json"})
    cold, _ = call("cold_configure_build", "faset_build")
    call("warmup_unchanged_build", "faset_build", record=False)
    for _ in range(5):
        reused, _ = call("warm_unchanged_build", "faset_build")
        if not reused["result"].get("schema_cache_hit"):
            raise RuntimeError("An unchanged build did not hit the verified schema cache")

    original_cpp = gameplay.read_bytes()
    original_header = header.read_bytes()
    for index in range(6):
        gameplay.write_bytes(original_cpp +
                             f"\n// P1 C++ iteration {index}\n".encode("utf-8"))
        source_hash = remember(gameplay, f"cpp-edit-{index}")
        stale, _ = call("schema_stale_check", "faset_schema_status",
                        source_hash=source_hash, record=False)
        if not stale["stale"]:
            raise RuntimeError("A C++ source edit did not mark gameplay metadata stale")
        label = "warmup_changed_cpp_build" if index == 0 else "changed_cpp_build"
        call(label, "faset_build", source_hash=source_hash, record=index != 0)
    for index in range(6):
        header.write_bytes(original_header +
                           f"\n// P1 header iteration {index}\n".encode("utf-8"))
        source_hash = remember(header, f"header-edit-{index}")
        label = "warmup_changed_header_build" if index == 0 else "changed_header_build"
        call(label, "faset_build", source_hash=source_hash, record=index != 0)

    working_header = header.read_bytes()
    header.write_bytes(working_header + b"\n#error FASET_P1_EXPECTED_BUILD_FAILURE\n")
    failed_hash = remember(header, "intentional-failure")
    run("build_failure", [editor, "--project", project, "--command",
                          json.dumps({"name": "faset_build", "arguments": {}}), "--wait"],
        source_hash=failed_hash, expected_failure=True)
    header.write_bytes(working_header)
    recovery_hash = remember(header, "recovery")
    built, _ = call("build_recovery", "faset_build", source_hash=recovery_hash)

    settings = json.loads((project / "project.faset.json").read_text(encoding="utf-8"))
    scene = project / settings["start_scene"]
    player = Path(built["result"]["player"])
    player_profiles = []
    for index in range(5):
        profile = raw / f"cpp-first-frame-{index + 1:02d}.json"
        run("player_one_frame_process", [player, "--scene", scene, "--assets",
                                         project / ".faset/cache", "--headless", "--frames",
                                         "1", "--profile", profile],
            source_hash=recovery_hash)
        profile_data = json.loads(profile.read_text(encoding="utf-8"))
        if profile_data["presentation_mode"] != "offscreen" or \
                profile_data["completed_frames"] != 1:
            raise RuntimeError("The first-frame profile is not one rendered offscreen frame")
        player_profiles.append(profile_data)
        samples.setdefault("offscreen_first_rendered_frame", []).append({
            "seconds": profile_data["startup_ms"]["main_to_first_frame"] / 1000,
            "build_configuration": "Debug", "source_hash": recovery_hash,
            "presentation_mode": profile_data["presentation_mode"],
            "profile": str(profile.relative_to(output))})

    # The UI probe measures synthetic keyboard input through snapshot rendering,
    # with no display presentation claim.
    ui_profile = raw / "editor-input-latency.json"
    run("editor_ui_probe_process", [ui_binary, ui_profile], source_hash=recovery_hash)
    ui_data = json.loads(ui_profile.read_text(encoding="utf-8"))
    if ui_data["presentation_mode"] != "offscreen" or \
            ui_data["measured_frames"] != len(ui_data["samples"]):
        raise RuntimeError("UI probe lost raw frames or mislabelled presentation")
    samples["editor_input_visible_frame"] = [
        {"seconds": row["milliseconds"] / 1000, "build_configuration": "Debug",
         "source_hash": recovery_hash, "presentation_mode": "offscreen",
         "input": row["gizmo"], "profile": str(ui_profile.relative_to(output))}
        for row in ui_data["samples"]]

    original_lua = lua_file.read_bytes()
    lua_build, _ = call("lua_initial_build", "faset_build", root=lua_project,
                        source_hash=lua_baseline_hash)
    lua_player = Path(lua_build["result"]["player"])
    lua_scene = lua_project / lua_input["start_scene"]
    control = lua_project / ".faset/measurement-control.json"
    process = subprocess.Popen([str(lua_player), "--scene", str(lua_scene), "--project",
                                str(lua_project), "--control", str(control), "--watch-lua",
                                "--headless", "--frames", "10000000"], cwd=output,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               text=True, encoding="utf-8")
    events = queue.Queue()
    stderr_lines = []

    def collect_lua():
        for line in process.stderr:
            event = (time.perf_counter(), line)
            stderr_lines.append(event)
            events.put(event)

    reader = threading.Thread(target=collect_lua, daemon=True)
    reader.start()

    def wait_lua(marker, timeout=20):
        deadline = time.perf_counter() + timeout
        while time.perf_counter() < deadline:
            try:
                event = events.get(timeout=min(.2, max(.001, deadline - time.perf_counter())))
            except queue.Empty:
                if process.poll() is not None:
                    break
                continue
            if marker in event[1]:
                return event[0]
        raise RuntimeError(f"Lua Player did not report {marker}; see raw/lua-reload-stderr.txt")

    try:
        # Startup settles before the watch/reload timing begins.
        wait_lua("Lua", timeout=20)
        for index in range(6):
            lua_file.write_bytes(original_lua +
                                 f"\n-- P1 reload iteration {index}\n".encode("utf-8"))
            started = time.perf_counter()
            source_hash = remember(lua_file, f"reload-{index}", lua_project)
            finished = wait_lua("Lua reloaded:")
            if index:
                samples.setdefault("changed_lua_reload", []).append({
                    "seconds": finished - started, "build_configuration": "Debug",
                    "source_hash": source_hash, "presentation_mode": "offscreen",
                    "player_log": "raw/lua-reload-stderr.txt"})
        control.parent.mkdir(parents=True, exist_ok=True)
        staging = control.with_suffix(".tmp")
        staging.write_text(json.dumps({"sequence": 1, "command": "stop"}),
                           encoding="utf-8")
        staging.replace(control)
        process.wait(timeout=20)
        if process.returncode:
            raise RuntimeError("Lua Player failed during reload measurements")
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=3)
        reader.join(timeout=2)
        (raw / "lua-reload-stdout.txt").write_text(process.stdout.read(), encoding="utf-8")
        (raw / "lua-reload-stderr.txt").write_text("".join(line for _, line in stderr_lines),
                                                    encoding="utf-8")
        lua_file.write_bytes(original_lua)

    lifecycle_profile = raw / "cpp-3000-frame-lifecycle.json"
    run("resource_lifecycle_process", [player, "--scene", scene, "--assets",
                                       project / ".faset/cache", "--headless", "--frames",
                                       "3000", "--profile", lifecycle_profile],
        source_hash=recovery_hash)
    lifecycle = json.loads(lifecycle_profile.read_text(encoding="utf-8"))
    frames = lifecycle["samples"]
    if len(frames) != 3000:
        raise RuntimeError("Resource lifecycle did not complete 3,000 frames")
    memory_start = max(row["gpu_allocated_bytes"] for row in frames[100:200])
    memory_end = max(row["gpu_allocated_bytes"] for row in frames[-100:])
    resource_growth = {"reference_frames": [101, 200], "end_frames": [2901, 3000],
                       "explicit_gpu_bytes_reference_max": memory_start,
                       "explicit_gpu_bytes_end_max": memory_end,
                       "explicit_gpu_bytes_growth": memory_end - memory_start,
                       "texture_count_reference_max": max(row["texture_count"]
                                                          for row in frames[100:200]),
                       "texture_count_end_max": max(row["texture_count"]
                                                    for row in frames[-100:])}

    generated_scene = generate_scene_fixture(json.loads(scene.read_text(encoding="utf-8")),
                                             seed=20260924, count=512)
    large_scene = raw / "generated-512.scene.json"
    large_scene.write_text(json.dumps(generated_scene, ensure_ascii=False, indent=2) + "\n",
                           encoding="utf-8")
    large_hash = remember(large_scene, "generated-512", output)
    large_profile = raw / "generated-512-profile.json"
    run("larger_generated_scene_process", [player, "--scene", large_scene, "--assets",
                                            project / ".faset/cache", "--headless", "--frames",
                                            "240", "--profile", large_profile],
        source_hash=large_hash)
    large_data = json.loads(large_profile.read_text(encoding="utf-8"))

    engine = Path(__file__).resolve().parents[1]
    revision = subprocess.run(["git", "rev-parse", "HEAD"], cwd=engine,
                              capture_output=True, text=True, check=True).stdout.strip()
    dirty = subprocess.run(["git", "status", "--porcelain"], cwd=engine,
                           capture_output=True, text=True, check=True).stdout.strip()
    cpu = platform.processor()
    if Path("/proc/cpuinfo").exists():
        cpu = next((line.split(":", 1)[1].strip() for line in
                    Path("/proc/cpuinfo").read_text().splitlines()
                    if line.startswith("model name")), cpu)
    ram_kib = None
    if Path("/proc/meminfo").exists():
        ram_kib = next((int(line.split()[1]) for line in
                        Path("/proc/meminfo").read_text().splitlines()
                        if line.startswith("MemTotal:")), None)

    def version(arguments):
        try:
            return subprocess.run(arguments, capture_output=True, text=True, timeout=15,
                                  encoding="utf-8").stdout.splitlines()[0]
        except (OSError, subprocess.TimeoutExpired, IndexError):
            return None

    report = {
        "format": "faset.workflow-measurements", "version": 2,
        "recorded_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "revision": revision, "working_tree_dirty": bool(dirty),
        "source_hashes": source_hashes, "project_hashes_at_copy": project_hashes,
        "build_configuration": "Debug", "source_project": str(source),
        "lua_source_project": str(lua_source), "editor": str(editor),
        "host": {"platform": platform.platform(), "cpu": cpu, "ram_kib": ram_kib,
                 "logical_cpus": os.cpu_count(), "python": platform.python_version()},
        "toolchain": {"cmake": version(["cmake", "--version"]),
                      "cxx": version(["clang++", "--version"]),
                      "slang": version(["slangc", "-version"])},
        "gpu": {"player_device": player_profiles[0]["device"],
                "ui_device": ui_data["device"],
                "driver": version(["nvidia-smi", "--query-gpu=driver_version",
                                   "--format=csv,noheader"])},
        "settings": {"resolution": [player_profiles[0]["width"],
                                     player_profiles[0]["height"]],
                     "validation_enabled": player_profiles[0]["validation_enabled"],
                     "readback_enabled": True, "presentation_mode": "offscreen",
                     "source_scene_entities": len(json.loads(scene.read_text())["entities"]),
                     "generated_scene_entities": len(generated_scene["entities"])},
        "samples": samples,
        "summary_seconds": {label: summarize([row["seconds"] for row in rows])
                            for label, rows in samples.items()},
        "resource_lifecycle": resource_growth,
        "large_scene_summary_ms": large_data["summary_ms"],
        "coverage": {"windowed_first_presented_frame":
                     "unmeasured; only offscreen rendered frames were sampled",
                     "reference_release_budgets":
                     "not compared with this Debug development workflow"},
        "method": [
            "Fresh disposable C++ and Lua project copies; checked-in sources never modified.",
            "Cold build means empty project .faset caches, not cold OS cache or dependency download.",
            "Warm/changed C++ and header builds have one warm-up then five measured repetitions.",
            "Lua edit-to-reload uses a persistent watched Player, one warm-up and five edits; "
            "wall time includes up to 500 ms watcher polling and stderr notification.",
            "First frame is Player main-to-first-completed offscreen frame; OS loader excluded.",
            "UI latency uses 10 warm-up and 100 keyboard-input to offscreen-render frames; "
            "there is no window presentation timing claim.",
            "The 3,000-frame lifecycle compares maximum explicit Vulkan allocation and texture "
            "counts in frames 101-200 and 2901-3000; it is not a whole-process leak proof.",
            "Times are observations under ambient host load, not CI pass/fail thresholds."]}
    validate_report(report)
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"Report: {output / 'report.json'}", flush=True)
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
