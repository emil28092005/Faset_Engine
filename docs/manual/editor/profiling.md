# Profiling and measurements

Use a Release export to measure the shipping Player. Record the exact scene, hardware,
driver, build configuration and resolution with the result. Small test scenes do not
establish performance for a large game.

## Capture a bounded Player profile

From a standalone generation directory:

```sh
./faset_player --headless --frames 240 --profile profile.json
```

`--headless` here means **offscreen Vulkan rendering**. A GPU/driver is still required.
The Editor's headless authoring mode is a separate feature. Omit this flag to measure
the windowed path. `--profile` requires an explicit `--frames` between 1 and 100000,
which bounds the stored samples.

The JSON contains raw completed-frame samples and nearest-rank p50/p95 summaries.
No warm-up frames are silently removed. It records the presentation mode, device,
resolution, validation activation, fixed ticks and timestep. A bounded run advances
one synthetic fixed timestep per frame; it does not reproduce a real-time input
session. Keep that distinction when comparing runs.

Startup starts at the Player application entry after platform argument normalization
and ends at the first completed frame. OS process loading and Windows `wmain` UTF-8
argument conversion are excluded. Frame wall times exclude writing the final profile and
capture files. Simulation and scene-snapshot times are separate from the renderer
call. Renderer CPU wall duration includes GPU waits and readback; it is **not CPU
utilization**. GPU timestamps measure the submitted graphics work and can be null
when timestamps are unsupported.

Resource counters report live renderer allocations and texture count. GPU allocation
bytes include Vulkan allocation alignment and exclude driver-internal memory; they
are not a whole-process VRAM meter. The fallback white texture is included.

Use `--debug-physics` or press **F3** to show current physics box colliders. Debug
geometry increases draw count, so record whether it was enabled. The collider view
uses simulation poses; normal visuals can use interpolated poses.

## Measure Editor, C++ and Lua iteration

From the engine repository:

```sh
cmake --build --preset linux-debug --target faset_editor faset_editor_ui_latency --parallel 2
python3 tools/measure_workflows.py \
  --editor build/linux-debug/faset_editor \
  --project examples/projects/collect-3d \
  --output /tmp/faset-workflow-measurement
```

The UI probe must be built beside the Editor first. The default Lua input is
`examples/lua`; use `--lua-project` to select another declared Lua project and
`--ui-latency-binary` when the probe is elsewhere. Use a **new** output directory;
the tool refuses to overwrite evidence. It copies both projects and never edits
the checked-in examples.

`report.json` version 2 separates the initial cold project configure/build,
unchanged builds, changed `.cpp` builds, changed-header builds, deliberate compile
failure/recovery, Lua edit-to-reload, first offscreen Player frame and synthetic
Editor input-to-visible-state. Each warm/changed case has one warm-up and five
measured repetitions. Each sample remains in `raw/` with command, stdout/stderr,
duration and, on Linux, peak RSS; the report includes median and nearest-rank p95.
The file hashes identify every changed source variant. Five unchanged builds must
return a verified schema/package cache hit. The cold case begins with empty project
`.faset` caches, **not** a cold OS file cache, newly downloaded dependencies or a
fresh engine toolchain. It is a development `Debug` workflow.

The first-frame value comes from the Player's `main_to_first_frame` profile after
five one-frame offscreen runs. It excludes OS process loading and says nothing about
window presentation latency. The separate UI probe applies keyboard input through
the retained Editor UI, then measures 100 post-warm-up offscreen frames; it is not
native desktop input-to-photon latency. A watched Lua Player records five
edit-to-successful-reload times, including its polling and log notification. A
3,000-frame Player profile compares explicit Vulkan allocation and texture maxima
in frames 101–200 with frames 2901–3000. Zero growth over that interval is useful
but does not prove the absence of all leaks. The tool also adds 512 deterministic
objects to a disposable scene and profiles 240 frames; this stresses scene
simulation/snapshot work, not representative game content.

On Linux, GNU `time` records peak RSS for each command and its waited-for children.
This is a maximum, not the sum of simultaneous compiler processes. Other platforms
report this field as null unless equivalent measurement support is added. The report
keeps raw stdout/stderr, profiles, hardware, driver, build setting, source hashes,
Git revision and dirty-tree state. New runs also hash the exact CMake, Ninja, C/C++
compiler, Slang, Editor and UI-probe binaries selected by the disposable project's
`CMakeCache.txt`. Compare profiles only with the same scene, configuration,
resolution, validation/readback settings and hardware class. The
[dated P1 workflow record](https://github.com/emil28092005/Faset_Engine/blob/main/docs/validation/p1-iteration-2026-09-24/README.md)
retains a successful Debug run and its exact raw files. Its post-run binary
provenance supplement is explicitly separate from the original report.

`tools/verify_playable_exports.py` separately verifies the two sample games in
relocated Release packages and records their Player profiles. Its assertions test
correct execution, not a frame-time threshold.

## Compare P2 GPU visibility modes

The Editor diagnostics panel (**F12**) can switch its current viewport between
**Direct**, **GPU frustum**, and **GPU occlusion**. Direct is the default reference.
The selector is an Editor viewport setting; it does not change the saved scene or
automatically change an exported Player. An exported Player can select a mode for a
bounded run:

```sh
./faset_player --headless --frames 240 --profile gpu-frustum.json --visibility gpu-frustum
```

Accepted values are `direct`, `gpu-frustum`, and `gpu-occlusion`; Direct is the
default. The profile records the requested `visibility_mode`, the run's and each
frame's `effective_visibility_mode`, and each frame's `gpu_visibility_active` state.
Compare requested and effective modes before interpreting a GPU run: a missing GPU
profile falls back to Direct, while missing HZB can reduce GPU occlusion to GPU
frustum. The Editor shows the **Effective path** and any **Fallback from** line. See
[Diagnostics](diagnostics.md) for the counters and HZB preview.

For a repeatable offscreen comparison, build and run the P2 benchmark harness:

```sh
build/linux-debug/faset_render_gpu_acceptance_tests --benchmark /tmp/faset-p2.csv
```

It records 10 warm-up and 30 measured frames for Direct, GPU frustum and GPU
occlusion in fixed frustum-heavy, open and occluded scenes. Run it three times and
compare median/p95 by scene and mode. Keep the raw CSV, hardware/driver, resolution,
shader bundle, validation state and source revision with any published result. The
[P2 acceptance protocol](https://github.com/emil28092005/Faset_Engine/blob/main/docs/studies/19-p2-gpu-visibility-acceptance.md)
documents the scenes and CSV columns. The
[first measured report](https://github.com/emil28092005/Faset_Engine/blob/main/docs/studies/20-p2-gpu-visibility-benchmark-2026-09-23.md)
is a **pre-optimization baseline**: its Debug/validation profile found GPU MainCull
substantially more expensive than direct GPU work. The
[optimized follow-up](https://github.com/emil28092005/Faset_Engine/blob/main/docs/studies/21-p2-gpu-visibility-optimization-2026-09-23.md)
retains three additional raw runs and isolates the effects of device-local output
buffers and bounded atomic append. MainCull p50 fell to 0.030–0.042 ms in those
synthetic scenes. That comparison is useful for diagnosis, not a guarantee that
GPU visibility speeds up a particular game or device.

The harness enables GPU visibility counters, so diagnostic readback is part of its
timings. In the Editor, opening diagnostics likewise enables these counters, and
**Show HZB** adds an on-demand image copy and preview upload. Close the panel and
disable the HZB preview for ordinary gameplay timing. GPU pass timestamps separate
MainCull, MainRaster, HZB, PostCull and PostRaster when supported; they are not a
measure of CPU extraction/upload. The renderer still waits for frame completion
and reads back the full image, so `cpu_ms` is wall time including waits, not CPU
utilization. An open scene can run slower with HZB; visibility correctness and
full-frame speed are separate findings.

## Compare temporal modes

Use one scene, output resolution, camera sequence, visibility path, binary and GPU
for Off, TAA and Upscale. Run enough frames to include both the first-frame reset
and steady-state accumulation. Keep the raw captures as well as timing samples:

```sh
./faset_player --headless --frames 240 --profile off.json --temporal off
./faset_player --headless --frames 240 --profile taa.json --temporal taa
./faset_player --headless --frames 240 --profile upscale.json \
  --temporal upscale --render-scale 0.67
```

The profile records requested and effective temporal modes, fallback and history
reset reason, internal/output extent, jitter, and valid previous-transform count
per completed frame. `gpu_temporal_resolve_ms`, `gpu_temporal_composite_ms`, and
`gpu_ui_ms` are separate submitted GPU pass times when timestamp queries work;
otherwise they are `null`. `gpu_allocated_bytes` includes live temporal targets
and histories, subject to the allocation limits described above. Compare full
frame GPU and renderer wall time too: scene raster savings can be offset by
resolve, memory and synchronous readback. A valid frame-level history flag says
the previous frame may be sampled, not that every pixel accepted it. For image
quality, inspect a still thin edge, a slow pan and a newly uncovered surface, and
compare the same frame against Off. See [Temporal rendering](temporal.md) for
mode controls and native C++ configuration.

## Measure P3 lighting and shadows

A Player `--profile` sample includes `effective_lighting_path`, local lights
submitted/omitted, requested/effective sun cascades, requested/rasterized local
shadow faces, tile use, shadow drop reasons, caster draws, and explicit atlas
allocation bytes. `gpu_main_raster_ms`, `gpu_sun_shadow_ms`, and
`gpu_local_shadow_ms` are GPU timestamps or `null` when timestamps are
unavailable. A light can illuminate while its shadow faces are dropped. A
submitted-light count of zero is a different workload from 128 lights whose
shadows are disabled. See [Lighting](lighting.md) for the capacity policy and
[Diagnostics](diagnostics.md) for the Editor counters.

The same sample includes `effective_lighting_path` (`forward` or `tiled`),
`gpu_light_tiles_ms`, and `light_tile_count`. Stored candidate and overflow
counts are present only when visibility diagnostics readback was enabled;
`light_tile_counts_valid: false` means their `null` values are unavailable,
not zero. The normal `Auto` setting currently resolves to `forward` after the
fixed dense 1080p benchmark showed that tile construction cost outweighed its
raster savings. A C++ renderer integration can explicitly request `Tiled` for
a localized-light scene, then check the actual path before comparing timings.

The fixed-scene benchmark compares 0, 4, 16, 32, 64, and 128 local lights under
Direct, GPU frustum, and GPU occlusion visibility, with shadows on and off. Its
wrapper runs three independent 1920×1080 repetitions per configuration, each
with ten warm-up and thirty recorded frames. First inspect the planned matrix:

```sh
python3 tools/benchmark_p3_lighting.py --list-runs
```

From the repository, after a Linux Release renderer build, run one shadow setting
into a new output directory. Supply the actual device driver identity:

```sh
python3 tools/benchmark_p3_lighting.py --sweep \
  --executable build/linux-release/faset_p3_lighting_benchmark \
  --output .cache/p3-lighting-off \
  --shadows off --driver 'REPLACE_WITH_ACTUAL_DRIVER' --validation off
```

The wrapper writes one raw CSV per run, `merged.csv`, and `summary.json`. Keep
all three with the exact source revision and device. It checks that every run
used its requested visibility mode and submitted every requested light. GPU
timestamps for the main raster isolate fragment-heavy lighting better than
renderer wall time, which includes GPU waits and synchronous readback. Shadow
time is split into sun and local GPU durations. The Forward+ decision compares
the median of three run medians against the matching zero-light configuration;
the threshold is **1.0 ms extra main raster time or 15% of the zero-light GPU
frame** at 32, 64, or 128 lights on the Linux physical reference GPU. The
[P3 lighting validation record](https://github.com/emil28092005/Faset_Engine/blob/main/docs/validation/p3-lighting-2026-09-24/README.md)
states the measured decision and scope. A software Vulkan run checks
functionality, not physical GPU performance.

For a direct comparison of the two algorithms on the same scene, invoke the
Release executable twice with `--lighting forward` and `--lighting tiled`,
using the same `--lights`, `--shadows`, `--visibility`, and output size. The
default `--light-layout dense` preserves the fixed benchmark scene;
`--light-layout localized` reduces point-light ranges to 1.75 units as a
separately labelled workload. Compare `gpu_build_plus_raster_ms`, which includes
`gpu_light_tiles_ms`, rather than raster time alone. One optional diagnostic
frame with `--tile-diagnostics on` reports candidate and overflow counts but
adds a GPU readback, so do not mix it into the timed runs. The
[Forward+ measurement](https://github.com/emil28092005/Faset_Engine/blob/main/docs/studies/23-p3-forward-plus-2026-09-24.md) retains
raw frames, shader hashes, and the decision.

## Current performance scope

The accepted MVP path uses direct draws and CPU culling; P2 adds optional GPU
visibility for opaque static meshes, with prepared LODs supplied by the project.
P3 adds local lights and bounded sun/local shadow atlases. The benchmark's
`lighting_path` and a Player profile's `effective_lighting_path` identify the
algorithm actually used. Both paths currently use one graphics queue and
synchronous full-image capture/readback. Use measurements to find the next
bottleneck before introducing parallel jobs or expanding GPU-driven rendering.
Neither an offscreen capture benchmark nor a tiny demo is a promise of a
production frame budget. Observed measurements and follow-up targets belong in
the implementation acceptance report with their source revision and method.
