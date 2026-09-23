#!/usr/bin/env python3
"""Export the checked-in playable projects through the real Editor and run relocated games.

Requires a built Editor, native C++ build tools, Slang and a Vulkan 1.3 driver.
Uses --headless for offscreen rendering. Windows CI can use the pinned SwiftShader
setup; the report records whether Khronos validation was actually available.
Output must be new or empty. Relocated games are retained in an external temporary
directory, and captures/profiles/logs are copied into the evidence directory.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def read_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def write_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def vulkan_device_details(summary: str, device: str) -> dict:
    """Return the driver identity from vulkaninfo for the Player-selected device."""
    for section in re.split(r"(?m)^GPU\d+:\s*$", summary)[1:]:
        fields = dict(re.findall(
            r"(?m)^\s*(apiVersion|driverVersion|driverID|driverName|driverInfo|deviceName)"
            r"\s*=\s*(.+?)\s*$", section))
        if fields.get("deviceName") == device:
            return fields
    return {}


def run(arguments: list[str | Path], cwd: Path, log: Path, timeout: int = 1800) -> dict:
    command = [str(argument) for argument in arguments]
    print(f"{log.stem}: {subprocess.list2cmdline(command)}", flush=True)
    started = time.monotonic()
    process = subprocess.Popen(command, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               text=True, encoding="utf-8", errors="replace",
                               start_new_session=os.name != "nt",
                               creationflags=subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0)
    timed_out = False
    try:
        stdout, stderr = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
        if os.name == "nt":
            subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
        else:
            os.killpg(process.pid, signal.SIGKILL)
        stdout, stderr = process.communicate()
    record = {"arguments": command, "cwd": str(cwd), "seconds": time.monotonic() - started,
              "exit_code": process.returncode, "timed_out": timed_out,
              "stdout": stdout, "stderr": stderr}
    write_json(log, record)
    require(not timed_out and process.returncode == 0,
            f"Command failed; inspect {log}: {stderr[-2000:]} {stdout[-2000:]}")
    return record


def command(editor: Path, engine: Path, project: Path, name: str, arguments: dict, log: Path) -> dict:
    response = run([editor, "--engine", engine, "--project", project, "--command",
                    json.dumps({"name": name, "arguments": arguments}), "--wait"], engine, log)
    result = json.loads(response["stdout"])
    require(result.get("state") == "succeeded", f"Editor job did not succeed: {log}")
    return result


def relative_path(root: Path, relative: str) -> Path:
    path = (root / relative).resolve()
    require(path.is_relative_to(root.resolve()), f"Package path escapes its root: {relative}")
    return path


def verify_package(directory: Path) -> dict:
    manifest = read_json(directory / "manifest.json")
    require(manifest.get("format") == "faset.export" and manifest.get("version") == 1,
            "Unsupported export manifest")
    require(manifest["configuration"] == "Release", "Playable exports must use Release")
    for entry in manifest["files"]:
        path = relative_path(directory, entry["path"])
        require(path.is_file() and not path.is_symlink(), f"Missing packaged file: {path}")
        require(path.stat().st_size == entry["size"] and sha256(path) == entry["sha256"],
                f"Packaged file checksum mismatch: {path}")
    return manifest


def verify_capture(path: Path, *, language: str = "cpp") -> dict:
    header, dimensions, maximum, pixels = path.read_bytes().split(b"\n", 3)
    require(header == b"P6" and maximum == b"255", "Expected RGB PPM screenshot")
    width, height = (int(value) for value in dimensions.split())
    require(width > 0 and height > 0 and len(pixels) == width * height * 3,
            "Incomplete screenshot")
    step = 3 * max(1, width * height // 10000)
    colors = len({pixels[index:index + 3] for index in range(0, len(pixels), step)})
    # The checked-in Lua sample deliberately has four flat palette colors.
    require(colors >= (4 if language == "lua" else 6),
            "Screenshot lacks the expected game geometry/colors")
    if language == "lua":
        sampled = {pixels[index:index + 3] for index in range(0, len(pixels), step)}
        require(any(r > 220 and g > 120 and b < 100 for r, g, b in sampled) and
                any(r < 100 and g > 150 and b > 180 for r, g, b in sampled),
                "Lua screenshot lacks its gold collectible or cyan player")
    return {"width": width, "height": height, "sampled_colors": colors, "sha256": sha256(path)}


def project_matrix(include_lua: bool, only_lua: bool) -> list[tuple[str, int, str]]:
    """Keep the existing two-game default; explicitly opt in to the Lua export."""
    if only_lua:
        return [("lua", 2, "lua")]
    projects = [("collect-2d", 2, "cpp"), ("collect-3d", 3, "cpp")]
    if include_lua:
        projects.append(("lua", 2, "lua"))
    return projects


def project_workspace(engine: Path, output: Path) -> Path:
    """Native builds must never be staged under the engine source tree."""
    if output.is_relative_to(engine):
        return Path(tempfile.mkdtemp(prefix="faset-playable-projects-"))
    return output


def verify_lua_export(directory: Path, manifest: dict) -> None:
    require(manifest.get("lua_enabled") is True, "Lua export did not enable the Lua runtime")
    scripts = [entry["path"] for entry in manifest["files"]
               if entry["path"].startswith("Scripts/") and entry["path"].endswith(".lua")]
    require(scripts and all((directory / path).is_file() for path in scripts),
            "Lua export has no packaged gameplay source")
    require("Notices/lua/LICENSE.txt" in {entry["path"] for entry in manifest["files"]} and
            (directory / "Notices/lua/LICENSE.txt").is_file(),
            "Lua export is missing its license notice")
    require(not (directory / "Scripts/Gameplay.cpp").exists() and
            not (directory / "Scripts/Gameplay.hpp").exists(),
            "Lua-only export unexpectedly contains C++ gameplay source")
    require(not (directory / ".luarc.json").exists(),
            "Lua-only export includes Editor language-server configuration")


def main() -> int:
    # Keep captured CI logs portable even when the Windows console uses a legacy code page.
    sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--editor", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--standalone-root", type=Path,
                        help="New/empty directory outside the engine and output trees")
    parser.add_argument("--include-lua", action="store_true",
                        help="Also export, relocate, validate and render examples/lua")
    parser.add_argument("--only-lua", action="store_true",
                        help="Run only the Lua project (useful for focused local verification)")
    args = parser.parse_args()
    engine, editor, output = args.engine.resolve(), args.editor.resolve(), args.output.resolve()
    require(editor.is_file(), f"Editor does not exist: {editor}")
    require(not output.exists() or not any(output.iterdir()), "Output directory must be new/empty")
    output.mkdir(parents=True, exist_ok=True)
    standalone = (args.standalone_root.resolve() if args.standalone_root else
                  Path(tempfile.mkdtemp(prefix="faset-playable-exports-")))
    require(not standalone.is_relative_to(engine) and not standalone.is_relative_to(output),
            "Standalone games must be outside the engine and evidence trees")
    require(not standalone.exists() or not any(standalone.iterdir()),
            "Standalone root must be new/empty")
    standalone.mkdir(parents=True, exist_ok=True)
    workspace = project_workspace(engine, output)
    evidence, projects = output / "evidence", workspace / "Faset Café 世界"
    evidence.mkdir()
    projects.mkdir()
    revision = subprocess.run(["git", "-C", str(engine), "rev-parse", "HEAD"],
                              capture_output=True, text=True, check=True).stdout.strip()
    vulkan_summary = ""
    if shutil.which("vulkaninfo"):
        try:
            probe = subprocess.run(["vulkaninfo", "--summary"], capture_output=True,
                                   text=True, encoding="utf-8", errors="replace", timeout=30)
            if probe.returncode == 0:
                vulkan_summary = probe.stdout
                (evidence / "vulkaninfo-summary.txt").write_text(
                    probe.stdout + probe.stderr, encoding="utf-8")
        except (OSError, subprocess.TimeoutExpired):
            pass
    standalone_projects = standalone / "Faset Café 世界"
    standalone_projects.mkdir()
    matrix = project_matrix(args.include_lua, args.only_lua)
    report = {"format": "faset.playable-export-verification", "version": 1,
              "started_utc": datetime.now(timezone.utc).isoformat(), "platform": sys.platform,
              "engine": str(engine), "editor": str(editor), "standalone_root": str(standalone),
              "engine_revision": revision, "editor_sha256": sha256(editor),
              "project_workspace": str(workspace),
              "vulkaninfo_available": bool(vulkan_summary),
              "driver_override": os.environ.get("VK_DRIVER_FILES") or os.environ.get("VK_ICD_FILENAMES"),
              "frames_per_game": 120, "status": "running", "projects": [],
              "requested_projects": [name for name, _, _ in matrix]}
    disabled = workspace / "projects-offline"
    try:
        for name, dimension, language in matrix:
            source = engine / "examples" / ("lua" if language == "lua" else f"projects/{name}")
            project = projects / name
            shutil.copytree(source, project, ignore=shutil.ignore_patterns(".faset", "Exports", "*.blend1"))
            inputs = [{"path": path.relative_to(project).as_posix(), "sha256": sha256(path)}
                      for path in sorted(project.rglob("*")) if path.is_file()]
            settings = read_json(project / "project.faset.json")
            scene = read_json(relative_path(project, settings["start_scene"]))
            item = {"name": name, "dimension": dimension, "language": language,
                    "source_inputs": inputs}
            report["projects"].append(item)
            if dimension == 3:
                imported = command(editor, engine, project, "faset_import",
                                   {"path": "Assets/exit-arch/manifest.json"}, evidence / f"{name}-import.json")
                item["import"] = imported["result"]
            exported = command(editor, engine, project, "faset_export",
                               {"document": scene["id"], "output": "Exports"},
                               evidence / f"{name}-export.json")
            pointer = read_json(project / "Exports/current.json")
            require(pointer["format"] == "faset.export-pointer" and pointer["version"] == 1,
                    "Invalid current export pointer")
            generation = relative_path(project / "Exports", pointer["directory"])
            require(generation == Path(exported["result"]["directory"]).resolve(),
                    "Export response and published pointer disagree")
            manifest = verify_package(generation)
            if language == "lua":
                verify_lua_export(generation, manifest)
            require(dimension != 3 or bool(manifest["asset_generations"]),
                    "3D game did not package its imported Blender asset")
            relocated = standalone_projects / name
            shutil.copytree(generation, relocated)
            verify_package(relocated)
            if language == "lua":
                verify_lua_export(relocated, manifest)
            shutil.copy2(relocated / "manifest.json", evidence / f"{name}-manifest.json")
            item.update({"generation": pointer["generation"], "configuration": "Release",
                         "standalone_directory": str(relocated), "executable": manifest["executable"],
                         "package_file_count": len(manifest["files"]),
                         "asset_generations": manifest["asset_generations"]})
            write_json(output / "report.json", report)
        # Hide exactly our disposable source-project paths while launching the games.
        # An empty, unrelated cwd also catches assumptions about the current directory.
        projects.rename(disabled)
        working = standalone / "empty-working-directory"
        working.mkdir()
        for item in report["projects"]:
            require(not projects.exists(), "Source project paths must be unavailable during launch")
            name, dimension = item["name"], item["dimension"]
            relocated = Path(item["standalone_directory"])
            player = relative_path(relocated, item["executable"])
            validation = run([player, "--validate"], working, evidence / f"{name}-validate.json", 120)
            require(json.loads(validation["stdout"]).get("validated") is True,
                    "Standalone CPU validation failed")
            capture, profile = relocated / "verification.ppm", relocated / "profile.json"
            rendered = run([player, "--headless", "--frames", "120", "--capture", capture,
                            "--profile", profile], working, evidence / f"{name}-run.json", 300)
            summary = json.loads(rendered["stdout"].strip().splitlines()[-1])
            require(summary["frames"] == 120 and summary["dimension"] == dimension and
                    summary["validation_errors"] == 0, "Standalone GPU run reported an error")
            measured = read_json(profile)
            require(measured["format"] == "faset.player-profile" and measured["version"] == 1 and
                    measured["completed_frames"] == 120 and measured["dimension"] == dimension and
                    measured["presentation_mode"] == "offscreen" and measured["validation_errors"] == 0 and
                    len(measured["samples"]) == 120, "Invalid or incomplete frame profile")
            require(all(frame["gpu_allocated_bytes"] > 0 for frame in measured["samples"]),
                    "Frame profile lacks Vulkan allocation measurements")
            item.update({"device": measured["device"], "validation_enabled": measured["validation_enabled"],
                         "driver": vulkan_device_details(vulkan_summary, measured["device"]),
                         "validation_errors": 0, "completed_frames": 120,
                         "summary_ms": measured["summary_ms"],
                         "capture": verify_capture(capture, language=item["language"]),
                         "source_project_paths_unavailable": True, "status": "passed"})
            shutil.copy2(capture, evidence / f"{name}.ppm")
            shutil.copy2(profile, evidence / f"{name}-profile.json")
            write_json(output / "report.json", report)
        report["status"] = "passed"
    except Exception as error:
        report.update({"status": "failed", "error": str(error)})
        raise
    finally:
        if disabled.exists() and not projects.exists():
            disabled.rename(projects)
        report["finished_utc"] = datetime.now(timezone.utc).isoformat()
        write_json(output / "report.json", report)
    print(f"{len(matrix)} relocated Release game(s) passed: {output / 'report.json'}",
          flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
