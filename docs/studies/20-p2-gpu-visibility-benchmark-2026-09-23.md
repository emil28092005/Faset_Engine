# P2 GPU visibility: first measured run (2026-09-23)

The [acceptance protocol](19-p2-gpu-visibility-acceptance.md) passed all 15 offscreen Vulkan cases on this source state. Three benchmark repetitions then produced 810 measured frames and zero Vulkan validation errors. GPU visibility reduced CPU render-call time in these synthetic scenes, but its GPU command time was substantially higher than the direct path. The compute `MainCull` pass accounts for almost all of that GPU cost. This is a Debug build with validation and diagnostic counter readback, so the numbers identify work to profile; they do not establish Release-build throughput.

## Reproduction and scope

| Field | Value |
| --- | --- |
| Source commit | `ae537c0a4b28e6033a6db045d3401f8d8e96f2ee` |
| Acceptance executable SHA-256 | `8eba12dab776ee64a2edcdf743f3bad83b91a991608578ee136c072566ab4751` |
| `src/render/renderer.cpp` SHA-256 | `d650718b78cb757d322586aef3ebfc97cb412dd36ac29d3071886f59d045752e` |
| `shaders/gpu_scene.slang` SHA-256 | `8fb7460ebcabc87a844fa2774f3bdad1d1f594a8d3b84d0a6881c9e439a35d98` |
| Compiled SPIR-V manifest SHA-256 | `307a13cf30c8211703224072d7d730035dea2f8eaf0e66f0a70e01737ce72238` (SHA-256 of lexically sorted `sha256sum build/linux-debug/shaders/*.spv` output) |
| Build | CMake Debug, Ubuntu Clang 21.1.8, Linux 7.0.0-31-generic x86_64 |
| GPU / driver | NVIDIA GeForce RTX 2080 Ti / 595.84; Vulkan device API 1.4.329 |
| Capture | Headless 320 × 240 RGBA, synchronous image readback every frame |
| Diagnostics | Vulkan validation and `visibility_diagnostics` enabled; zero reported validation errors |
| Workload | Deterministic geometry, no random seed; 1,024 grid instances plus scene-specific occluder; shadows disabled for benchmark objects |
| Sampling | Sequential direct → GPU frustum → GPU occlusion for each scene; 10 warmup + 30 sampled frames per scene/mode/run; three sequential runs |

The three raw, unmodified per-frame captures are [run 1](data/p2-2026-09-23-run-1.csv), [run 2](data/p2-2026-09-23-run-2.csv), and [run 3](data/p2-2026-09-23-run-3.csv). Each contains 270 rows: 30 frames for each of three scenes and three visibility modes. Source, shader and executable hashes were the same before and after all three runs. The CSV `frame` field resets to zero for each scene/mode. The SPIR-V manifest hash above includes file names and hashes in sorted order.

Values below pool 90 frames per scene/mode. P50 is the median and p95 is linearly interpolated at rank `(n - 1) × 0.95` after sorting. The run range is the minimum–maximum of the three *individual-run p50* values, making run-to-run variation visible. Times are milliseconds.

| Scene | Mode | CPU render p50 / p95 | CPU run-p50 range | GPU commands p50 / p95 | GPU run-p50 range |
| --- | --- | ---: | ---: | ---: | ---: |
| Frustum-heavy | Direct | 73.60 / 90.94 | 71.37–81.42 | 0.241 / 0.268 | 0.240–0.242 |
| Frustum-heavy | GPU frustum | 14.58 / 23.04 | 13.48–15.23 | 4.00 / 4.69 | 3.89–4.12 |
| Frustum-heavy | GPU occlusion | 15.01 / 22.14 | 14.00–15.67 | 3.99 / 4.60 | 3.96–4.02 |
| Open | Direct | 70.34 / 89.71 | 68.70–71.51 | 0.502 / 0.531 | 0.500–0.503 |
| Open | GPU frustum | 29.52 / 37.88 | 28.98–30.67 | 18.08 / 19.21 | 17.87–18.25 |
| Open | GPU occlusion | 30.82 / 38.47 | 29.11–33.36 | 18.36 / 19.18 | 18.03–18.63 |
| Occluded | Direct | 68.00 / 84.26 | 67.45–68.64 | 0.499 / 0.554 | 0.496–0.500 |
| Occluded | GPU frustum | 29.63 / 40.42 | 28.49–30.71 | 18.26 / 19.23 | 18.15–18.36 |
| Occluded | GPU occlusion | 30.39 / 37.37 | 29.16–31.10 | 18.24 / 19.32 | 17.86–18.47 |

`cpu_ms` spans the synchronous `Renderer::render` call, including CPU preparation, submission, GPU completion wait, and host image copy; it excludes renderer construction and the ten warmup frames. `gpu_ms` is the Vulkan timestamp interval around recorded GPU commands, including the image-to-buffer capture copy, but excludes later host mapping and copying. The median isolated host `readback_cpu_ms` is 0.05–0.06 ms in these runs. Neither column is a Player frame rate, and subtracting them does not isolate CPU culling cost.

## Passes, work saved, and memory

The table shows pass-level GPU p50 / p95, in milliseconds. A dash means the pass is absent in that mode. These timestamp spans include work recorded under the named pass and are not interchangeable with the whole-frame GPU interval.

| Scene | Mode | MainCull | MainRaster | HZB | PostCull | PostRaster |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| Frustum-heavy | Direct | — | 0.195 / 0.198 | — | — | — |
| Frustum-heavy | GPU frustum | 3.917 / 4.624 | 0.029 / 0.050 | — | — | — |
| Frustum-heavy | GPU occlusion | 3.869 / 4.484 | 0.026 / 0.046 | 0.033 / 0.040 | 0.005 / 0.006 | 0.006 / 0.007 |
| Open | Direct | — | 0.455 / 0.464 | — | — | — |
| Open | GPU frustum | 17.956 / 19.120 | 0.046 / 0.084 | — | — | — |
| Open | GPU occlusion | 18.230 / 19.036 | 0.046 / 0.083 | 0.033 / 0.036 | 0.005 / 0.005 | 0.006 / 0.007 |
| Occluded | Direct | — | 0.453 / 0.511 | — | — | — |
| Occluded | GPU frustum | 18.174 / 19.142 | 0.047 / 0.085 | — | — | — |
| Occluded | GPU occlusion | 18.115 / 19.202 | 0.018 / 0.019 | 0.033 / 0.035 | 0.024 / 0.028 | 0.006 / 0.007 |

The visibility counters are stable across the 90 sampled frames of each GPU scene/mode:

| Scene | Candidate count | GPU main-visible | Frustum rejected | HZB deferred | Post-visible | Direct / GPU-frustum / GPU-occlusion draw calls |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Frustum-heavy | 1,024 | 416 | 608 | 0 | 0 | 416 / 1 / 2 |
| Open | 1,024 | 1,024 | 0 | 0 | 0 | 1,024 / 1 / 2 |
| Occluded | 1,026 | 1 in occlusion mode | 0 | 1,025 in occlusion mode | 0 | 1,026 / 1 / 2 |

The direct path does not produce GPU visibility counters; zeros in its CSV rows mean “not measured,” not zero visible objects. Peak `FrameStats::gpu_allocated_bytes` was about 7.45 MiB for direct, 5.17 MiB for GPU frustum, and 6.50 MiB for GPU occlusion in this harness. This counter is the renderer's tracked allocations, not whole-process GPU memory.

The frustum fixture removes 608 main-view candidates and cuts main raster time, while the occluded fixture defers 1,025 candidates and cuts its main raster time. Yet the current `MainCull` cost exceeds those savings by an order of magnitude on this device/configuration. The next performance investigation should isolate compute dispatches, transfer/barrier and indirect-buffer work inside `MainCull`, then repeat in Release with validation and diagnostic counter readback disabled. The fixed mode order and GPU clock/thermal state may affect absolute values; a randomized or interleaved follow-up would reduce order bias. The existing acceptance tests verify image agreement in memory, but this benchmark does not store reference images or measure visual quality over real game content.
