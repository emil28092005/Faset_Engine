# P3 light scaling and Forward+ decision (2026-09-24)

The first Release measurement of P3 lighting crosses the planned Forward+ gate. On the Linux reference RTX 2080 Ti, 32 local lights added about 0.50 ms of forward raster work, over 28% of the zero-light GPU frame in every visibility mode. At 64 lights the added raster time was about 1.02 ms. The result requires a measured tiled Forward+ implementation under [the P3 plan](../superpowers/plans/2026-09-24-p3-lighting.md); it does not establish a speedup until that implementation is compared on the same workload.

## Reproduction and scope

| Field | Recorded value |
| --- | --- |
| Source revision | `05906054cf5c4f6a9346ce0b4c507f256566e43e`, clean at the start and end of the sweep |
| Benchmark executable SHA-256 | `946850c2354e848a72212a4f15292c8e5564747f4bc60e010a307904d38db8e3` |
| Host and device | Linux, NVIDIA GeForce RTX 2080 Ti; NVIDIA driver 595.84; physical Vulkan GPU selected by the renderer |
| Build | Clang/CMake `linux-release`, 1920 × 1080, Vulkan validation disabled for timing |
| Workload | Fixed camera, ten meshes, 0/4/16/32/64/128 point lights; local shadows both off and on; Direct, GPU frustum and GPU occlusion |
| Sampling | Ten warm-up and thirty measured frames per run, three runs for each of 36 configurations: 108 runs, 3,240 raw frame rows |

The exact command was:

```sh
python3 tools/benchmark_p3_lighting.py --sweep \
  --executable build/linux-release/faset_p3_lighting_benchmark \
  --output /tmp/faset-p3-release-2026-09-24 \
  --driver NVIDIA-595.84 --validation off
```

The [summary](data/p3-lighting-2026-09-24/forward/summary.json), [merged per-frame CSV](data/p3-lighting-2026-09-24/forward/merged.csv), and [individual raw files](data/p3-lighting-2026-09-24/forward/raw/shadows-off_direct_lights-000_run-1.csv) retain the counters and acquisition order. All 108 individual files are in the adjacent `raw/` directory. The wrapper rejects a dirty source checkout, a changed benchmark binary, a Debug executable, mixed devices/drivers/lighting paths, visibility fallback, validation errors, nonzero omitted-light counts, or inconsistent shadow-face counts. The driver name supplied to the benchmark was cross-checked against `nvidia-smi` and `vulkaninfo`; the device name is reported by Vulkan. This remains a single-machine synthetic scene measurement.

## Gate result

Each configuration's p50 is the median of three run medians. The overhead compares each high-light run with its matching zero-light repeat under the same visibility and shadow settings, then takes the median of the three differences. For GPU occlusion, the raster metric adds main and post raster passes. The gate is reached if a 32-, 64-, or 128-light overhead is at least 1.0 ms **or** 15% of the matching zero-light whole GPU frame.

| Shadow setting | Visibility | Added raster at 32 lights | At 64 lights | At 128 lights |
| --- | --- | ---: | ---: | ---: |
| Off | Direct | 0.502 ms (31.0%) | 1.012 ms (62.4%) | 2.033 ms (125.5%) |
| Off | GPU frustum | 0.507 ms (32.3%) | 1.017 ms (64.8%) | 2.030 ms (129.4%) |
| Off | GPU occlusion | 0.507 ms (28.9%) | 1.020 ms (58.2%) | 2.028 ms (115.7%) |
| On | Direct | 0.534 ms (35.3%) | 1.046 ms (69.1%) | 2.074 ms (137.0%) |
| On | GPU frustum | 0.538 ms (34.9%) | 1.056 ms (68.5%) | 2.077 ms (134.7%) |
| On | GPU occlusion | 0.539 ms (30.7%) | 1.057 ms (60.2%) | 2.079 ms (118.4%) |

All 3,240 frames submitted the requested local-light count, reported zero Vulkan errors, and retained the requested visibility mode. Validation was **off** during the performance sweep; zero reported errors here is not a validation-layer pass. Separate GPU tests exercise validation and image correctness.

With shadows on, four point lights request 24 faces but only 12 fit atomically in the 16-tile atlas (two full six-face point lights). The same 12 faces remain at 16–128 lights; 128 lights request 768 faces and explicitly drop 756. The corresponding local shadow GPU p50 stays near 0.018 ms. Thus the scaling above is mainly fragment shading, not additional shadow-map rendering. The table does not imply that 128 shadowed points are supported simultaneously.

`gpu_ms` includes the renderer's synchronous framebuffer capture copy, while `cpu_ms` includes submission and wait. Neither is an interactive game frame rate. The gate deliberately uses GPU raster timestamps; the post-occlusion raster pass is included when active. The fixed geometry and light arrangement, GPU clock state, and one driver limit generalization to other scenes and devices. The retained raw data allow this conclusion to be recomputed without trusting the prose.

The next P3 checkpoint must implement depth-free 16 × 16 tiled Forward+ with a full-light fallback for overflowing tiles, compare rendered RGB against the current full-scan path, and remeasure tile-build **plus** raster time on this same workload. The simple path remains available as a correctness reference until that result is recorded.
