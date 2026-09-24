# P3 lighting and shadows — acceptance record

This record tracks P3 lighting separately from temporal reconstruction. The
implementation and measured Forward+ checkpoint is source revision
`a0a4e29d480ed3344f19bd3565d48668ca913fed` on `feat/p3-lighting`.
The earlier shadow/benchmark integration checkpoint was `b191ae0`. The
lighting slice has Linux functional and reference-GPU evidence; Windows CI for
the new tiled revision is pending. Temporal reconstruction has its own acceptance.

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
- An explicit 16×16 depth-free tiled Forward+ path uses at most 64 light indices
  per tile and evaluates the complete submitted list on overflow. The compute
  entry has checked reflection and is included in game builds. `Auto` uses
  forward: three-run Release measurements found tile build + raster slower on
  the dense fixed scene. The [paired study](../../studies/23-p3-forward-plus-2026-09-24.md)
  retains a separate localized-light win and exact binary/shader provenance.

## Acceptance matrix

| Case | Automated evidence | Current status |
| --- | --- | --- |
| Empty, disabled, local-only, multiple sun; schema bounds | `scene_view`, `render_lighting_policy`, `render_offscreen` | Linux Debug green at `a0a4e29`; integrated revision pending |
| Four cascades, split bounds, subtexel stabilization, offscreen/source-LOD0 caster | `render_lighting_policy`, `render_lighting_sun` | Linux GPU and pinned SwiftShader P3 green at `a0a4e29` |
| Spot cone, six point faces and seam, dropped whole point shadow | `render_lighting_local`, `render_lighting_policy` | Linux GPU and pinned SwiftShader P3 green at `a0a4e29` |
| 128-light/16-face/4096-draw limits, unsupported-atlas fallback | `render_lighting_policy`, `render_offscreen`, `render_lighting_local` | CPU and supported-atlas GPU paths covered; actual unsupported Vulkan device not tested |
| Direct/GPU frustum/GPU occlusion image parity, P2 reload and 2D/UI independence | `render_lighting_sun`, `render_lighting_local`, `render_shader_reload`, `render_offscreen` | Linux Debug green at `a0a4e29`; integrated revision pending |
| Forward+/forward parity, near plane, resize, overflow, and shader reload | `render_lighting_tiled`, `render_shader_reload`, `render_shader_reflection`, `build_schema_publication` | Linux Debug and pinned SwiftShader P3 green at `a0a4e29`; 128-light localized Release captures match exactly |
| Driver, profile, real 64×64 benchmark smoke | `render_lighting_benchmark_schema`, `render_lighting_benchmark_smoke`, `player_shutdown_diagnostics` | Full Linux Debug green at `a0a4e29` |
| 1920×1080 0/4/16/32/64/128 Release sweep, three repeats, both shadow states | `tools/benchmark_p3_lighting.py --sweep` | Forward baseline measured; its separate raw study is being integrated |
| 1920×1080 paired paths, 32/64/128 dense and localized lights | `faset_p3_lighting_benchmark --lighting forward|tiled` | Raw 1080 frames and six diagnostic samples retained in study 23; dense slower, localized faster by build+raster |
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
`render_lighting_local`, `render_lighting_tiled`, and
`render_lighting_benchmark_smoke` (labelled `gpu;p3`). Use
`ctest --test-dir build/linux-debug -N -L p3` to confirm those six cases exist
before running them; an empty test selection is not a pass.
The Windows full graphics job runs all registered tests, while the native
Windows CPU job uses `-LE gpu` and therefore excludes the four Vulkan cases.

On the Linux host at `b191ae0`, the [CTest inventory](linux-debug-p3-inventory.txt)
listed all five cases. The [CPU-only P3 run](linux-debug-cpu-ctest.txt) passed
`render_lighting_policy` and `render_lighting_benchmark_schema` 2/2 with zero
failures. The [strict MkDocs build](strict-mkdocs.txt) passed for these Manual
changes. This run deliberately excluded Vulkan tests while the 1920×1080
physical-GPU baseline was being measured, so it is not a final GPU acceptance
result. The local host was Linux x86_64, kernel 7.0.0-31-generic; the source
checkout had documentation changes only during these checks.

At `a0a4e29`, the [full Linux Debug run](linux-debug-tiled-ctest.txt) had
63 registered cases: 62 passed, no failures, and the compositor-dependent
window lifecycle case skipped. The [pinned Linux SwiftShader P3 run](linux-swiftshader-tiled-p3-ctest.txt)
passed all six P3 cases without a skip. The Vulkan image cases requested
validation and asserted zero reported errors. The RTX 2080 Ti A/B used NVIDIA
driver 595.84.0.0; study 23 records the executable and shader bundle hashes,
all raw per-frame timings, tile overflow counts, and exact image equality for
the localized 128-light capture. Its first dense 32-light forward run was an
outlier, so the decision uses the median of three process medians rather than
the apparent win in one paired run.

At the earlier `b191ae0` checkpoint, [GitHub native/manual CI](https://github.com/emil28092005/Faset_Engine/actions/runs/35935899512)
and [Windows graphics/SwiftShader CI](https://github.com/emil28092005/Faset_Engine/actions/runs/35935899505)
passed. These jobs did **not** include the new tile shader; Windows CI for
`a0a4e29` is still required.

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
driver and paired path data are retained in study 23. Release full CTest,
final integrated Windows Actions and relocated Player checks still need to be
added before this is a complete P3 lighting acceptance record.

## Limits carried forward

The default path scans all submitted lights in each mesh fragment; the
measured Forward+ threshold prompted a bounded tiled implementation. `Auto`
still uses forward because this dense fixed workload was slower after tile
construction. Transparent/game UI and sprites keep their existing
ordering and unlit behavior. The atlas caps are fixed budgets, not adaptive
quality settings, and shadow depth is redrawn each frame. The renderer still
performs synchronous framebuffer readback. No broad scene/driver matrix or
physical Windows GPU performance claim follows from these fixtures.
