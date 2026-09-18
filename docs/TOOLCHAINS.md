# Toolchain and execution profiles

Faset uses C++20 without compiler extensions. Dependencies are pinned by commit and
archive SHA-256 in `dependencies.lock.json`; Slang is pinned to **2026.18** by
`tools/fetch_slang.py`. Changing these inputs is a deliberate SDK change. Editor
plugins additionally require the exact generated build fingerprint, including
compiler, platform, CRT, configuration, dependencies and SDK source identity.

## Observed compiler profiles

- **Linux development:** Ubuntu 26.04, x86-64, Clang **21.1.8**, CMake **4.2.3**,
  Ninja, glibc **2.43**. Native Editor, GPU tests, gameplay and exports execute here.
- **Linux CI:** Ubuntu 24.04, x86-64, Clang **18.1.3**. The headless CPU suite and
  manual run on this profile. It is not evidence for desktop rendering on that runner.
- **Windows CI:** Windows Server 2025 runner, x86-64, clang-cl **20.1.8**,
  MSVC toolset **14.51.36231**, Windows SDK **10.0.26100.0**. Headless tests have
  passed. Full Editor, software Vulkan and Release-package acceptance are tracked
  separately in the implementation log until that job completes.

These are recorded validation profiles, not a claim that every intermediate Clang
release or every supported Windows desktop has been tested. The presets deliberately
use the compiler available on PATH so local installations remain practical. CI
configuration output records the actual detected versions; runner image upgrades
must be reviewed against this baseline. CMake **3.25** is the declared minimum, not
the exact version used in every recorded run.

MSVC-family builds use the dynamic CRT: `/MDd` in Debug, `/MD` otherwise. Physics,
Player, Editor and native plugins use compatible settings. Development and export
have separate CMake directories, avoiding accidental Debug/Release mixing.

## Graphics profiles

The baseline requires Vulkan 1.3 with dynamic rendering and synchronization2, a
graphics queue, the renderer's color/depth format usages and limits, and presentation
support for a native window. The backend checks these requirements before creating
resources. It does not require hardware ray tracing, mesh shaders or descriptor
indexing. Timestamp availability is queried; unavailable GPU timings are reported
as unavailable rather than zero.

- **Physical Linux GPU:** NVIDIA GeForce RTX 2080 Ti, driver **595.84**, reported
  Vulkan **1.4.329**. Tests request `VK_LAYER_KHRONOS_validation` and report whether
  the layer actually activated. This machine runs both Wayland window and offscreen
  scenarios.
- **Software Vulkan:** official SwiftShader and Vulkan Loader sources are pinned
  with archive hashes in `tools/ci/prepare_windows_vulkan.py`. This profile checks
  API execution and output pixels on Windows CI. It is CPU rendering and is not a
  physical GPU performance benchmark. The bootstrap does **not** install validation
  layers, so its zero error counter must not be described as validation-layer proof.

The measured physical GPU is one verified device, not an exhaustive compatibility
matrix. Additional AMD/Intel GPUs, Windows desktop drivers and display configurations
need their own execution records.

## Offline preparation

Before disconnecting, install the native toolchain and platform development packages,
fetch Slang, and run:

```sh
python3 tools/fetch_dependencies.py
python3 tools/fetch_dependencies.py --verify-only
```

Preserve `.cache/downloads`, `.cache/slang`, compilers, CMake/Ninja, Python and system
SDK/libraries. A fresh source checkout can then configure and build without fetching
third-party sources. Runtime exports require only their documented target runtime
and graphics driver; they do not need this development toolchain.

An offline build is recorded only after a clean build directory is configured and
built with network access disabled. A warm incremental build or archive checksum
verification alone is not that acceptance check.
