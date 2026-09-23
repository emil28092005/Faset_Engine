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

## Measure Editor and C++ workflows

From the engine repository:

```sh
python3 tools/measure_workflows.py \
  --editor build/linux-debug/faset_editor \
  --project examples/projects/collect-3d \
  --output .cache/my-workflow-measurement
```

Use a new output directory. The tool copies the project, preserving your original,
and records command startup, two-frame GUI startup/shutdown, first/cached Blender
bundle import, initial/no-change/changed Debug builds and a subsequent Player frame.
It checks that editing gameplay makes the schema stale and successful building
clears that state. The initial build uses available dependency archives and OS
caches; it is not a measurement of internet download speed.

On Linux, GNU `time` records peak RSS for each command and its waited-for children.
This is a maximum, not the sum of simultaneous compiler processes. Other platforms
report this field as null unless equivalent measurement support is added. The tool
keeps raw stdout/stderr, durations, hardware and revision information alongside its
report. A dirty source checkout is explicitly identified.

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
