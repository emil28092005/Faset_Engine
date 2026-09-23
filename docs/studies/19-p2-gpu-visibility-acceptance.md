# P2 GPU visibility: acceptance and benchmark protocol

This protocol exercises the post-MVP GPU-driven renderer against the retained direct path. It is an implementation and measurement checklist, not a performance claim. The prototype architecture and failure scenarios are described in [study 15](15-renderer-implementation-notes.md).

## Correctness gate

`cmake/GpuVisibilityAcceptance.cmake` registers fifteen offscreen Vulkan cases. Each renders a controlled `Snapshot` through `VisibilityMode::Direct` and a GPU mode, compares the expected final RGB image with a small tolerance for floating-point raster differences, and requires zero Vulkan validation errors. GPU cases also require `FrameStats::gpu_visibility_active`; a silent direct fallback cannot pass. Acceptance enables `RendererConfig::visibility_diagnostics`, which reads GPU counters back for assertions.

| Case | Regression caught |
| --- | --- |
| `empty` | A previous frame's indirect count leaks into an empty scene. |
| `capacity` | Instance ID / indirect-count allocation drops an instance at 64→65 candidates or after shrinking. |
| `dense` | GPU frustum rejects visible objects or fails to reject offscreen objects; bins degenerate to one CPU draw per instance. |
| `door` | Previous-frame HZB hides an object after its occluder moves; post-cull must reveal it in the same final frame. |
| `shadow` | Camera-frustum rejection incorrectly removes an offscreen shadow caster. The fixture first proves the caster's shadow affects the reference image. |
| `cut` | A camera cut reuses incompatible HZB history. |
| `resize` | Odd-sized target recreation retains stale HZB extents or reads out of bounds. |
| `lod` | A prepared coarse mesh is not selected at distance, small scale jitter switches it back, or a large scale change fails to restore the fine mesh. The direct reference explicitly renders the expected prepared mesh at each level. |
| `teleport` | A sudden camera move without an explicit cut marker leaves an object hidden; post-cull must recover it with the previous HZB still valid. |
| `near` | Frustum/HZB wrongly rejects bounds crossing the near plane or containing the camera. |
| `lifecycle` | Mass spawn/despawn, buffer growth, reused stable key, or replacement mesh reuses stale indirect counts or temporal bounds. |
| `views` | Two viewport IDs incorrectly share temporal HZB history. |
| `projection` | FOV/projection changes retain incompatible HZB history despite an unchanged view ID. |
| `open_sequence` | A long camera pan over an open scene loses otherwise visible instances or leaks stale history. |
| `transparent` | A transparent foreground draw contaminates the HZB and hides opaque geometry behind it. |

Run all acceptance tests after building:

```sh
ctest --test-dir build/linux-debug --output-on-failure -L p2
```

The pixel comparison allows at most 0.5% of pixels to differ by more than 16 RGB levels and a mean RGB error of at most 2. Capacity checks also inspect each cube center; the shadow case compares every pixel affected by the reference shadow. These targeted checks catch small omissions that a whole-frame tolerance could miss. A failing scene should be inspected by saving both full frames and a difference image before relaxing a threshold.

These cases are the automatic minimum. Manual follow-up should cover newly imported meshes, overlapping material bins, shader reloads, and final color plus depth/ID debug views where available. Assert no indirect buffer overflow or validation errors at the advertised capacity. Stable IDs must survive reordering; reused slots need a generation change. 2D sprites and editor UI retain their authored order.

## Benchmark command and interpretation

The standalone executable writes per-frame CSV without declaring a speedup:

```sh
build/linux-debug/faset_render_gpu_acceptance_tests --benchmark /tmp/faset-p2-dense.csv
```

It warms each mode for 10 frames, then records 30 frames for each of three fixed scenes: a frustum-heavy grid, an open scene with almost all instances visible, and a wall hiding a dense group. It repeats these workloads for direct, GPU frustum, and GPU occlusion. All benchmark objects disable shadows so shadow draw submission does not dominate the visibility comparison. The harness enables visibility diagnostics so that GPU counters are observable; this adds host mapping/readback overhead. Columns include whole-render-call CPU time, GPU command time, CPU readback time, pass-level GPU times (main cull/raster, HZB, post cull/raster), live Vulkan allocation bytes, submitted draw calls, visibility counters, LOD counts, and validation errors. Summarize median and p95 per scene/mode; retain the raw CSV. Run each configuration at least three times and report run-to-run variation. The harness intentionally has no performance assertion because gains depend on GPU, driver, scene and capture overhead.

The current renderer performs a full-image readback and waits for frame completion every frame. `cpu_ms` measures the whole synchronous `Renderer::render` call, including that wait and the host copy. `gpu_ms` measures the timestamp interval around GPU commands, including the image-to-buffer copy but excluding the later host map/copy. `readback_cpu_ms` measures only the host map/copy. Pass-level timestamps expose the cost of `MainCull`, `MainRaster`, `HZB`, `PostCull`, and `PostRaster`, but CPU extraction/upload/submission are not individually timed. The benchmark's diagnostic counter readback adds host work beyond a normal Player frame. Compare a closed, heavily occluded scene **and** an open scene in which most instances remain visible. Record identical geometry, camera path, window extent, shader bundle, validation setting and capture mode. Avoid benchmarking during shader compilation, texture uploads, first-use allocations or GPU frequency transitions.

Keep data from software Vulkan implementations separate from physical GPU data. A visibility feature is accepted by image correctness and lifecycle tests even if it is slower in a particular scene; retain the measured regression and decide whether to use the direct path there.

The first recorded three-run result, including source hashes, device metadata, p50/p95 tables, counter values, limitations, and links to all 810 raw frame records, is in [the 2026-09-23 benchmark report](20-p2-gpu-visibility-benchmark-2026-09-23.md). The GPU cull pass is much slower than the direct path's GPU work in this Debug/validation configuration, even as CPU submission time falls. Treat the result as a profiling lead, not a shipping performance verdict.
