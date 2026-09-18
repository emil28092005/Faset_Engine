#!/usr/bin/env python3
"""Build a pinned Vulkan loader and CPU driver for Windows graphics CI.

These are test tools, not engine or exported-game dependencies. This intentionally
uses upstream source archives rather than an unofficial prebuilt driver. It does
not install the Khronos validation layer; GPU pixel tests and packaging tests run
against SwiftShader; Khronos validation-layer runs are a separate check.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import urllib.request

SOURCES = {
    "headers": ("KhronosGroup/Vulkan-Headers", "0d3f509e57041fbd073a1ee84cd9ccd36d148446", "e82005a6bd3289ce213232ef41e0f8722d1365ed72bf5cc4be56911c56bc30e0"),
    "loader": ("KhronosGroup/Vulkan-Loader", "32fcb949e253cbeb40cda7ea76122b492db579ae", "610fa9017226c49bc4f19398d94fce9e3c9ff94c14f6420f9fe24822707feacf"),
    "swiftshader": ("google/swiftshader", "1e80438d2b93ef36a7c05f8d2b81233bac0e3d16", "1c3a1afc397c7aa4d3275790bc9b451e80b144a1db665135d5519838bacf44a8"),
}

# This exact previous helper cache used the same verified source pins. Allow a
# one-time migration without rebuilding LLVM, but never restore it after pin changes.
LEGACY_SOURCE_IDENTITY = "38d3f6735736a2d0cf25cec2d9edef8b1220f6d10d13a694efb633aa85c3b8fc"
LEGACY_CACHE_KEY = "windows-2025-vulkan-0ca023e20dd35e447b4e13537e95c70859cc7604367979f32c45705b3f370568"


def cache_identity() -> str:
    return hashlib.sha256(json.dumps(SOURCES, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def stamp(directory: Path, names: tuple[str, ...]) -> None:
    identity = {name: list(SOURCES[name]) for name in names}
    path = directory / "source-identity.json"
    if path.exists() and json.loads(path.read_text(encoding="utf-8")) != identity:
        raise RuntimeError(f"Cached tool source identity differs from the pins: {path}")
    path.write_text(json.dumps(identity, indent=2) + "\n", encoding="utf-8")


def run(*arguments: str | Path) -> None:
    command = [str(argument) for argument in arguments]
    print("+ " + subprocess.list2cmdline(command), flush=True)
    subprocess.run(command, check=True)


def source(cache: Path, name: str) -> Path:
    repository, commit, expected = SOURCES[name]
    directory = cache / "sources" / name
    if (directory / "CMakeLists.txt").is_file():
        return directory
    archives = cache / "downloads"
    archives.mkdir(parents=True, exist_ok=True)
    archive = archives / f"{name}-{commit}.tar.gz"
    if not archive.is_file():
        temporary = archive.with_suffix(".download")
        urllib.request.urlretrieve(f"https://codeload.github.com/{repository}/tar.gz/{commit}", temporary)
        temporary.replace(archive)
    with archive.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    if digest != expected:
        raise RuntimeError(f"{name} source archive checksum mismatch")
    extraction = cache / "sources" / f".{name}-extract"
    extraction.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive) as contents:
        contents.extractall(extraction, filter="data")
    roots = list(extraction.iterdir())
    if len(roots) != 1 or not roots[0].is_dir():
        raise RuntimeError(f"Unexpected source archive structure: {name}")
    roots[0].rename(directory)
    extraction.rmdir()
    return directory


def configure(source_path: Path, build: Path, *arguments: str) -> None:
    run("cmake", "-S", source_path, "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_C_COMPILER=cl", "-DCMAKE_CXX_COMPILER=cl", *arguments)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache", type=Path, default=Path(".cache/windows-graphics"))
    parser.add_argument("--loader-only", action="store_true", help="Build headers/loader only for CPU renderer-linked tests")
    parser.add_argument("--cache-key", action="store_true", help="Print source identity and expose Actions cache outputs without building")
    args = parser.parse_args()
    if args.cache_key:
        identity = cache_identity()
        key = "windows-2025-vulkan-v2-" + identity
        legacy = LEGACY_CACHE_KEY if identity == LEGACY_SOURCE_IDENTITY else ""
        print(key)
        if "GITHUB_OUTPUT" in os.environ:
            with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as output:
                output.write(f"key={key}\nlegacy={legacy}\n")
        return 0
    if sys.platform != "win32":
        parser.error("Run this helper on Windows in a Visual Studio x64 developer environment")
    for program in ("cmake", "ninja", "cl"):
        if not shutil.which(program):
            raise RuntimeError(f"Required tool missing: {program}")
    cache = args.cache.resolve()
    sdk, driver = cache / "sdk", cache / "driver"
    if not all((sdk / path).is_file() for path in ("bin/vulkan-1.dll", "lib/vulkan-1.lib", "include/vulkan/vulkan.h")):
        headers = source(cache, "headers")
        configure(headers, cache / "headers-build", f"-DCMAKE_INSTALL_PREFIX={sdk}", "-DVULKAN_HEADERS_ENABLE_TESTS=OFF", "-DVULKAN_HEADERS_ENABLE_MODULE=OFF")
        run("cmake", "--install", cache / "headers-build")
        loader = source(cache, "loader")
        configure(loader, cache / "loader-build", f"-DCMAKE_INSTALL_PREFIX={sdk}", f"-DCMAKE_PREFIX_PATH={sdk}", "-DBUILD_TESTS=OFF", "-DBUILD_WERROR=OFF")
        run("cmake", "--build", cache / "loader-build", "--parallel", "2")
        run("cmake", "--install", cache / "loader-build")
    stamp(sdk, ("headers", "loader"))
    if not args.loader_only and not (driver / "vk_swiftshader_icd.json").is_file():
        swift = source(cache, "swiftshader")
        build = cache / "swiftshader-build"
        configure(swift, build, "-DSWIFTSHADER_BUILD_TESTS=OFF", "-DSWIFTSHADER_BUILD_BENCHMARKS=OFF", "-DSWIFTSHADER_BUILD_PVR=OFF", "-DSWIFTSHADER_WARNINGS_AS_ERRORS=OFF")
        run("cmake", "--build", build, "--target", "vk_swiftshader", "--parallel", "2")
        manifest_directory = build / "Windows"
        manifest = json.loads((manifest_directory / "vk_swiftshader_icd.json").read_text(encoding="utf-8"))
        library = (manifest_directory / manifest["ICD"]["library_path"]).resolve()
        driver.mkdir(parents=True, exist_ok=True)
        shutil.copy2(library, driver / library.name)
        # Khronos' Windows loader identifies relative paths by a backslash, not
        # a forward slash. './name.dll' otherwise remains relative to the cwd.
        manifest["ICD"]["library_path"] = ".\\" + library.name
        (driver / "vk_swiftshader_icd.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    if not args.loader_only:
        # Repair the old separator on cache hits as well as newly built bundles.
        manifest_path = driver / "vk_swiftshader_icd.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        library_name = Path(manifest["ICD"]["library_path"]).name
        if not (driver / library_name).is_file():
            raise RuntimeError("Cached SwiftShader manifest points to a missing DLL")
        manifest["ICD"]["library_path"] = ".\\" + library_name
        manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        stamp(driver, ("swiftshader",))
    used_sources = {name: source for name, source in SOURCES.items() if not args.loader_only or name != "swiftshader"}
    report = {"sources": {name: {"repository": repository, "commit": commit, "sha256": digest} for name, (repository, commit, digest) in used_sources.items()}, "validation_layer": False, "vulkan_sdk": str(sdk), "driver": None if args.loader_only else str(driver / "vk_swiftshader_icd.json"), "loader_only": args.loader_only}
    cache.mkdir(parents=True, exist_ok=True)
    (cache / "toolchain.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if "GITHUB_ENV" in os.environ:
        with open(os.environ["GITHUB_ENV"], "a", encoding="utf-8") as output:
            output.write(f"VULKAN_SDK={sdk}\n")
            if report["driver"]:
                output.write(f"VK_DRIVER_FILES={report['driver']}\nVK_ICD_FILENAMES={report['driver']}\n")
        with open(os.environ["GITHUB_PATH"], "a", encoding="utf-8") as output:
            output.write(str(sdk / "bin") + "\n")
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
