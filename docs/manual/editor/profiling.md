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

## Current performance scope

The accepted MVP path uses direct draws and CPU culling; P2 adds optional GPU
visibility for opaque static meshes, with prepared LODs supplied by the project.
Both paths currently use one graphics queue and synchronous full-image
capture/readback. Use measurements to find the next bottleneck before introducing
parallel jobs or expanding GPU-driven rendering. Neither an offscreen capture
benchmark nor a tiny demo is a promise of a production frame budget. Observed
measurements and follow-up targets belong in the implementation acceptance report
with their source revision and method.
