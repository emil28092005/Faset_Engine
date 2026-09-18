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

Startup starts at `main()` and ends at the first completed frame. OS process loading
before `main()` is excluded. Frame wall times exclude writing the final profile and
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

## Current performance scope

The MVP renderer is intentionally conservative: direct draws, CPU culling, one
graphics queue and synchronous capture/readback. Use the measurements to find the
next bottleneck before introducing parallel jobs or GPU-driven rendering. Neither
an offscreen capture benchmark nor a tiny demo is a promise of a production frame
budget. Observed measurements and follow-up targets belong in the implementation
acceptance report with their source revision and method.
