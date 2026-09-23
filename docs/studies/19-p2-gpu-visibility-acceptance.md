# P2 GPU visibility: acceptance and benchmark protocol

This protocol exercises the post-MVP GPU-driven renderer against the retained direct path. It is an implementation and measurement checklist, not a performance claim. The prototype architecture and failure scenarios are described in [study 15](15-renderer-implementation-notes.md).

## Correctness gate

`cmake/GpuVisibilityAcceptance.cmake` registers eight offscreen Vulkan cases. Each renders a controlled `Snapshot` through `VisibilityMode::Direct` and a GPU mode, compares the expected final RGB image with a small tolerance for floating-point raster differences, and requires zero Vulkan validation errors. GPU cases also require `FrameStats::gpu_visibility_active`; a silent direct fallback cannot pass.

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

Run all acceptance tests after building:

```sh
ctest --test-dir build/linux-debug --output-on-failure -L p2
```

The pixel comparison allows at most 0.5% of pixels to differ by more than 16 RGB levels and a mean RGB error of at most 2. Capacity checks also inspect each cube center; the shadow case compares every pixel affected by the reference shadow. These targeted checks catch small omissions that a whole-frame tolerance could miss. A failing scene should be inspected by saving both full frames and a difference image before relaxing a threshold.

These cases are the automatic minimum. Before accepting occlusion or LOD, also run a scripted pan, teleport, near-plane crossing, spawned/deleted mesh, newly imported mesh, overlapping bins, two view IDs, and a long open-scene sequence. Compare final color and depth/ID debug views where available. Assert no indirect buffer overflow or validation errors at the advertised capacity. Stable IDs must survive reordering; reused slots need a generation change. 2D sprites and editor UI retain their authored order.

## Benchmark command and interpretation

The standalone executable writes per-frame CSV without declaring a speedup:

```sh
build/linux-debug/faset_render_gpu_acceptance_tests --benchmark /tmp/faset-p2-dense.csv
```

It warms each mode for 10 frames, then records 30 frames for each of three fixed scenes: a frustum-heavy grid, an open scene with almost all instances visible, and a wall hiding a dense group. It repeats these workloads for direct, GPU frustum, and GPU occlusion. All benchmark objects disable shadows so shadow draw submission does not dominate the visibility comparison. Columns include CPU and GPU frame times, CPU readback time, live Vulkan allocation bytes, submitted draw calls, visibility counters, LOD counts, and validation errors. Summarize median and p95 per scene/mode; retain the raw CSV. Run each configuration at least three times and report run-to-run variation. The harness intentionally has no performance assertion because gains depend on GPU, driver, scene and capture overhead.

The current renderer performs a full-image readback and waits for frame completion every frame. Its `cpu_ms` and `gpu_ms` describe that end-to-end implementation, not isolated culling cost. A claim that GPU culling itself is faster requires pass-level GPU timestamps for `MainCull`, `MainRaster`, `HZB`, `PostCull`, and `PostRaster`, alongside CPU extraction/upload/submission times. Compare a closed, heavily occluded scene **and** an open scene in which most instances remain visible. Record identical geometry, camera path, window extent, shader bundle, validation setting and capture mode. Avoid benchmarking during shader compilation, texture uploads, first-use allocations or GPU frequency transitions.

Record the following before publishing any result:

| Field | Result |
| --- | --- |
| Commit and shader bundle hash | _not measured_ |
| OS, GPU and driver | _not measured_ |
| Vulkan validation enabled | _not measured_ |
| Resolution and scene seed | _not measured_ |
| Visible / occluded / deferred instances | _not measured_ |
| Direct CPU p50 / p95 and GPU p50 / p95 | _not measured_ |
| GPU-frustum CPU p50 / p95 and GPU p50 / p95 | _not measured_ |
| GPU-occlusion CPU p50 / p95 and GPU p50 / p95 | _not measured_ |
| Pass-level GPU times | _not measured_ |
| Peak renderer allocation bytes | _not measured_ |
| Correctness images / validation errors | _not measured_ |

Keep data from software Vulkan implementations separate from physical GPU data. A visibility feature is accepted by image correctness and lifecycle tests even if it is slower in a particular scene; retain the measured regression and decide whether to use the direct path there.
