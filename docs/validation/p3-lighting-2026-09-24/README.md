# P3 lighting and shadows — acceptance record

This record tracks P3 lighting separately from temporal reconstruction. The
implementation checkpoint is source revision
`b191ae0bed77a1544a49504b9a3f07e9a3c691f2` on `feat/p3-lighting`. The
Manual/record edit itself is documentation-only. A later Forward+ change and
its measurements require a new revision and validation entry before the lighting
slice can be called complete. Temporal reconstruction has its own acceptance.

## Implemented at the checkpoint

- Authored directional, point, and spot lights are extracted from the same
  versioned Light schema used by the Inspector and MCP. No authored Light
  component retains the legacy sun; any authored Light, including disabled or
  local-only, suppresses that fallback.
- Direct, GPU frustum, and GPU occlusion graphics paths use the same typed
  lighting data. Material descriptors remain set 0, lighting set 1, and GPU
  graphics scene data set 2. Sun and local light contributions accumulate before
  tone mapping. Sprites and UI remain unlit.
- Four texel-snapped sun cascades cover up to 80 world units for an explicit
  camera frustum. A low-level Snapshot without one keeps a single sun view.
  Shadow caster selection uses each light view and source LOD 0, independent of
  camera visibility and prepared camera LOD. Missing/disabled sun and sprite-only
  scenes skip sun shadow raster.
- The separate D32 local atlas admits 16 faces, one per spot or six atomically
  per point. Both shadow systems share a 4096-caster-draw frame budget. Up to 128
  local lights are submitted by priority, projected influence, then stable ID.
  Overflow or unsupported-atlas lights remain unshadowed when submitted; omitted
  lights beyond 128 do not illuminate. Both atlases try 2048², then 1024².
- The editor overlay and Player profile expose actual submitted/omitted lights,
  requested/effective views, drop reasons, atlas bytes, caster draws, GPU shadow
  durations, and the effective lighting path. Shadow tiles are redrawn every
  frame; no persistent depth cache is claimed.

## Acceptance matrix

| Case | Automated evidence | Current status |
| --- | --- | --- |
| Empty, disabled, local-only, multiple sun; schema bounds | `scene_view`, `render_lighting_policy`, `render_offscreen` | Covered by Debug tests at the implementation checkpoint; re-run on final revision |
| Four cascades, split bounds, subtexel stabilization, offscreen/source-LOD0 caster | `render_lighting_policy`, `render_lighting_sun` | Covered by CPU policy and Linux Vulkan image tests; final-revision runs pending |
| Spot cone, six point faces and seam, dropped whole point shadow | `render_lighting_local`, `render_lighting_policy` | Covered by Linux Vulkan image and CPU tests; final-revision runs pending |
| 128-light/16-face/4096-draw limits, unsupported-atlas fallback | `render_lighting_policy`, `render_offscreen`, `render_lighting_local` | CPU and supported-atlas GPU paths covered; actual unsupported Vulkan device not tested |
| Direct/GPU frustum/GPU occlusion image parity, P2 reload and 2D/UI independence | `render_lighting_sun`, `render_lighting_local`, `render_shader_reload`, `render_offscreen` | Linux supported-driver paths covered; final-revision runs pending |
| Driver, profile, real 64×64 benchmark smoke | `render_lighting_benchmark_schema`, `render_lighting_benchmark_smoke`, `player_shutdown_diagnostics` | Focused integration tests passed on `b191ae0`; full raw log pending |
| 1920×1080 0/4/16/32/64/128 Release sweep, three repeats, both shadow states | `tools/benchmark_p3_lighting.py --sweep` | Baseline measured on Linux physical GPU; raw CSV and post-Forward+ comparison pending publication |
| Windows native build, pinned SwiftShader GPU tests, relocated Release 2D/3D Players | `windows-graphics.yml`, `ci.yml` | New P3 revision has not yet completed Windows CI |

The supported-atlas GPU tests create a renderer with validation requested and
assert zero reported Vulkan errors; a test result is a validation-layer pass only
when the layer was actually active. `render_window_lifecycle` can skip if the
Linux compositor declines programmatic restore. The Windows workflow uses pinned
SwiftShader, not a physical Windows GPU, and may lack the Khronos layer. Linux
reference-GPU results cannot establish physical Windows performance.

## Reproduction and retained evidence

The P3 CTest registrations are `render_lighting_policy` and
`render_lighting_benchmark_schema` (CPU), plus `render_lighting_sun`,
`render_lighting_local`, and `render_lighting_benchmark_smoke` (labelled
`gpu;p3`). Use `ctest --test-dir build/linux-debug -N -L p3` to confirm those
five cases exist before running them; an empty test selection is not a pass.
The Windows full graphics job runs all registered tests, while the native
Windows CPU job uses `-LE gpu` and therefore excludes the three Vulkan cases.

On the Linux host at `b191ae0`, the [CTest inventory](linux-debug-p3-inventory.txt)
listed all five cases. The [CPU-only P3 run](linux-debug-cpu-ctest.txt) passed
`render_lighting_policy` and `render_lighting_benchmark_schema` 2/2 with zero
failures. The [strict MkDocs build](strict-mkdocs.txt) passed for these Manual
changes. This run deliberately excluded Vulkan tests while the 1920×1080
physical-GPU baseline was being measured, so it is not a final GPU acceptance
result. The local host was Linux x86_64, kernel 7.0.0-31-generic; the source
checkout had documentation changes only during these checks.

```sh
cmake --build --preset linux-debug --parallel 2
ctest --test-dir build/linux-debug -L p3 --no-tests=error --output-on-failure
ctest --test-dir build/linux-debug --output-on-failure
cmake --build --preset linux-release --parallel 2
ctest --test-dir build/linux-release --output-on-failure
```

The benchmark wrapper retains one raw CSV per run, a merged CSV, and a summary.
It rejects visibility fallback, missing GPU timestamps, missing lights, duplicate
frames, and validation errors. An offscreen capture's `cpu_ms` includes GPU wait
and readback; it is not thread CPU time. The exact Release benchmark revision,
driver, CSV paths, before/after Forward+ gate, Linux SwiftShader results, final
Debug/Release CTest logs, and Windows Actions links will be added after those
checks run. Do not use this provisional record as a P3 completion claim.

## Limits carried forward

The current checkpoint scans all submitted lights in each mesh fragment; the
measured Forward+ threshold was reached on the Linux reference GPU, so a bounded
tiled path is in progress. Transparent/game UI and sprites keep their existing
ordering and unlit behavior. The atlas caps are fixed budgets, not adaptive
quality settings, and shadow depth is redrawn each frame. The renderer still
performs synchronous framebuffer readback. No broad scene/driver matrix or
physical Windows GPU performance claim follows from these fixtures.
