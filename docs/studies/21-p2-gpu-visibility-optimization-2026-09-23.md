# P2 GPU visibility: device-local outputs and bounded atomic append (2026-09-23)

The [first P2 benchmark](20-p2-gpu-visibility-benchmark-2026-09-23.md) found that `MainCull` took about 4 ms with 416 visible candidates and 18 ms with 1,024 candidates in one bin on an RTX 2080 Ti. The camera-visible raster pass was under 0.1 ms. We isolated two causes and measured them separately: GPU-written counters and IDs lived in host-cached system memory, and each contender retried a compare-and-exchange loop on one counter. With device-local outputs and one atomic fetch-add per valid append, the corresponding `MainCull` medians are 0.030 and 0.030 ms. These are measurements of the synthetic Debug/validation harness, not a general engine performance claim.

## Reproduction and provenance

| Field | Value |
| --- | --- |
| Baseline | Commit `ae537c0a4b28e6033a6db045d3401f8d8e96f2ee`; source/executable hashes and methodology in [study 20](20-p2-gpu-visibility-benchmark-2026-09-23.md) |
| Optimized implementation | Commit `3be3d0de5f54c7a9afcab48f6e969ad56273a6a4` |
| Optimized `src/render/renderer.cpp` SHA-256 | `0ebd863ff44fd7916244fb7b340407b27f50c048849c5df6c6f1f950b1d5dd1c` |
| Original/staged-CAS `shaders/gpu_scene.slang` SHA-256 | `8fb7460ebcabc87a844fa2774f3bdad1d1f594a8d3b84d0a6881c9e439a35d98` |
| Optimized `shaders/gpu_scene.slang` SHA-256 | `f58e86bfa8106364c70f26751f276793ca1dc02b34bab142c7ebea258e6102b3` |
| Staged/final acceptance executable SHA-256 | `4750257d8c6484ece781578f9275bd9e8eeadb487d6a6527693f08f36c80c9d8` (the shader is loaded separately at runtime) |
| Final SPIR-V manifest SHA-256 | `cac0fc853f106a2d15e15bf89b1b29136656e3936174369b91d61159cef023f2` (SHA-256 of lexically sorted `sha256sum build/linux-debug/shaders/*.spv` output) |
| Machine | Ubuntu Linux 7.0.0-31-generic, Clang 21.1.8, NVIDIA GeForce RTX 2080 Ti, driver 595.84 |
| Harness | CMake Debug, Vulkan validation and visibility diagnostics enabled, 320 × 240 synchronous capture; fixed direct → GPU-frustum → GPU-occlusion mode order |
| Samples | Three sequential runs per variant; 10 warmup + 30 measured frames per scene/mode/run; 810 measured rows per variant |

The device-local-only intermediate variant uses the optimized renderer source and the original compare-and-exchange shader. It therefore isolates memory placement before the final shader change. The raw, unmodified CSV captures are:

- Baseline: [run 1](data/p2-2026-09-23-run-1.csv), [run 2](data/p2-2026-09-23-run-2.csv), [run 3](data/p2-2026-09-23-run-3.csv).
- Device-local outputs with original CAS loop: [run 1](data/p2-2026-09-23-device-local-run-1.csv), [run 2](data/p2-2026-09-23-device-local-run-2.csv), [run 3](data/p2-2026-09-23-device-local-run-3.csv).
- Device-local outputs with atomic add: [run 1](data/p2-2026-09-23-optimized-run-1.csv), [run 2](data/p2-2026-09-23-optimized-run-2.csv), [run 3](data/p2-2026-09-23-optimized-run-3.csv).

P50 and p95 below pool 90 frames per scene/mode and use linear interpolation at sorted rank `(n - 1) × quantile`. All times are milliseconds. `gpu_ms` spans GPU commands, including the image-to-buffer capture copy; `cpu_ms` is the synchronous `Renderer::render` call, including submission and completion wait. Neither is a Player frame rate.

| Scene | Mode | Baseline GPU p50 / p95 | Device-local CAS GPU p50 / p95 | Final GPU p50 / p95 | Final CPU p50 |
| --- | --- | ---: | ---: | ---: | ---: |
| Frustum-heavy | Direct | 0.241 / 0.268 | 0.235 / 0.271 | 0.235 / 0.300 | 68.57 |
| Frustum-heavy | GPU frustum | 3.998 / 4.694 | 0.229 / 0.256 | 0.124 / 0.155 | 10.16 |
| Frustum-heavy | GPU occlusion | 3.986 / 4.597 | 0.292 / 0.314 | 0.187 / 0.196 | 9.62 |
| Open | Direct | 0.502 / 0.531 | 0.497 / 0.719 | 0.498 / 0.643 | 66.51 |
| Open | GPU frustum | 18.081 / 19.207 | 0.477 / 0.508 | 0.139 / 0.158 | 10.18 |
| Open | GPU occlusion | 18.359 / 19.183 | 0.544 / 0.558 | 0.204 / 0.238 | 9.39 |
| Occluded | Direct | 0.499 / 0.554 | 0.496 / 0.528 | 0.496 / 0.531 | 66.96 |
| Occluded | GPU frustum | 18.259 / 19.226 | 0.477 / 0.503 | 0.138 / 0.143 | 9.86 |
| Occluded | GPU occlusion | 18.237 / 19.320 | 0.532 / 0.539 | 0.188 / 0.212 | 10.61 |

The isolated `MainCull` pass provides a closer A/B for the two changes:

| Scene and mode | Baseline p50 / p95 | Device-local CAS p50 / p95 | Final p50 / p95 |
| --- | ---: | ---: | ---: |
| Frustum-heavy, GPU frustum (416 visible) | 3.917 / 4.624 | 0.135 / 0.140 | 0.030 / 0.047 |
| Open, GPU frustum (1,024 visible) | 17.956 / 19.120 | 0.368 / 0.373 | 0.030 / 0.032 |
| Occluded, GPU occlusion (1,025 deferred) | 18.115 / 19.202 | 0.385 / 0.391 | 0.042 / 0.044 |

All sampled visibility counts matched across the three variants: 416 frustum-visible, 1,024 open-visible, and 1,025 deferred with one main-visible in the occluded scene. All 2,430 measured frames across the three variants reported zero Vulkan validation errors. The final focused shader/reflection and GPU suite passed 18/18 tests, and `spirv-val --target-env vulkan1.3` accepted both culling shaders.

## Cause and implementation

The original `upload_scene_buffer` chose `HOST_VISIBLE | HOST_COHERENT` memory and preferred `HOST_CACHED` for every scene buffer. On this GPU, Vulkan memory type 4 has those flags on a non-device-local system-memory heap. The indirect command counters, deferred counter and visible-ID arrays were written by the GPU there. The shader also used a CAS retry loop to append into one bin: many invocations could read the same old count, one would advance it and the rest would retry. The occluded case spent about 18 ms in `MainCull` even though only one instance reached main raster, because 1,025 candidates contended on `deferredCount` instead.

The renderer now allocates GPU-written indirect args, visible IDs and deferred buffers with `DEVICE_LOCAL` memory. Host-visible staging buffers supply the two indirect templates and zeroed deferred count through transfer copies before compute. An explicit transfer-to-compute/indirect barrier makes those copies visible. Diagnostic counts are copied back to the same staging buffers after culling and raster, with compute/transfer and transfer/host barriers; ordinary rendering still avoids counter readback. This path does not require a host-visible device-local BAR heap. Tracked allocation size for this one-bin fixture increased by just 48 bytes for the three minimum-size staging buffers; the important change is memory placement.

The shader now uses one `InterlockedAdd` for each append. The host constructs each bin with capacity equal to its candidate count; each candidate index is dispatched once and can append at most once to main IDs. The separate post buffer receives only deferred candidates, also at most once each, and deferred capacity equals total candidate count. Thus valid generated input cannot overflow. A defensive overflow branch undoes the speculative increment before any draw consumes the count, retaining the bounded final indirect/deferred count if an invalid table is supplied.

These timings are from one NVIDIA GPU, one small synthetic workload and a Debug build with validation, diagnostics and synchronous image capture. The fixed mode order, clock state and driver scheduling can affect p95 values. The full Release build and other desktop GPUs remain separate validation; the direct path remains the runtime fallback. GPU occlusion adds HZB/post overhead in an open scene with nothing to hide, so applications should select it based on workload rather than treating this synthetic GPU-frustum gain as universal.
