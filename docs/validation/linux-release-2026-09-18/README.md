# Linux Release export baseline — 2026-09-18

Both checked-in playable projects passed real Editor export, standalone CPU validation and 120 offscreen frames after relocation. Their disposable source-project paths were hidden during launch; no `--scene` or `--assets` override was supplied. Copied project and executable paths contain `Faset Café 世界`. The 3D export includes the Blender exit-arch asset.

This is a measurement of the shared working tree around `5ac4db438dae5ee28e5619e62cf8d6647beecd52` plus in-progress checkpoint 4 changes. It is **not an exact clean-commit result**. The two Release generations are `48ec8546-3750-463f-9c4c-f76c3b933256` (2D) and `17281e17-9f4f-44f0-8821-6e0071e9a45b` (3D). [Evidence](evidence.json) retains project input hashes and build fingerprints; [2D](collect-2d-manifest.json) and [3D](collect-3d-manifest.json) manifests retain the exact executable, cooked data and shader hashes.

The same relocated executables then recorded 240 frames each, sequentially, without physics debug drawing or player input. Hardware: Ryzen 7 1700, 32 GiB RAM, RTX 2080 Ti 11 GiB, NVIDIA 595.84; Ubuntu 26.04, kernel 7.0.0-31. Build: Clang 21.1.8, Release `-O3 -DNDEBUG`, Vulkan 1.3, 1280×720, actual Khronos validation enabled. Both runs had zero validation errors. The desktop remained active; this was not an isolated benchmark machine.

Raw [2D](collect-2d-profile-240.json) and [3D](collect-3d-profile-240.json) profiles include every completed frame, including the first, with nearest-rank percentiles. Times below are p50 / p95 milliseconds:

- 2D: total measured frame 2.234 / 2.684; renderer call 1.865 / 2.221; GPU 0.565 / 0.579; readback 0.510 / 0.633; simulation 0.201 / 0.258; snapshot 0.170 / 0.215.
- 3D: total measured frame 2.299 / 2.732; renderer call 1.884 / 2.180; GPU 0.588 / 0.600; readback 0.469 / 0.608; simulation 0.213 / 0.300; snapshot 0.181 / 0.257.

Live Vulkan allocations stayed at 15,750,288 bytes (2D) and 15,787,008 bytes (3D), about 15.02 and 15.06 MiB. Counts were 11 / 31 draws, 66 / 576 packed vertices per frame, and one fallback texture. A packed mesh can be drawn again for its shadow pass. These memory totals include aligned explicit allocations and exclude driver internals. First-frame startup from `main()` was 265.5 / 260.8 ms; OS loader time is excluded.

`render_call` includes GPU waits and synchronous readback; it is not CPU utilization. GPU timestamps cover submitted rendering. Frame timings exclude profile bookkeeping and final file writes. Synthetic 1/60-second simulation ticks run as fast as the offscreen loop allows. The resulting numbers are not display FPS or a real-time gameplay pacing test. Small sample scenes, one GPU, 240 frames and a static camera cannot establish scalability, a memory-leak guarantee, or broad performance claims. The imported 3D material also emits the recorded warning about unsupported material features in its manifest.

Initial P1 tracking budgets for these exact scenes on this reference-class Linux host, at the same resolution and validation/readback settings:

- Measured frame p95 ≤ 4 ms; GPU p95 ≤ 1 ms; readback p95 ≤ 1 ms.
- Simulation and snapshot p95 ≤ 0.5 ms each.
- Explicit live Vulkan memory ≤ 20 MiB, with no growth after warmup over a future 3,000-frame resource-lifecycle run.
- Startup from `main()` ≤ 500 ms for these tiny packages.

These initial thresholds leave headroom above the observed values. They are tracking budgets for P1, not enforced MVP acceptance criteria or engine-wide guarantees. P1 should retain cold-start results separately, run repeated sessions, measure editor input latency and larger content, and collect independent Windows hardware baselines before adopting release gates.

Reproduce export validation from the repository with `python tools/verify_playable_exports.py --editor build/linux-debug/faset_editor --output <new-empty-directory>`. Run each retained standalone Player with `--headless --frames 240 --profile <output.json>` to repeat the longer measurement. A new run builds current sources and produces new immutable generations; use the stored hashes to distinguish it from this record.

Captured 120-frame outputs:

![Relocated 2D sample](collect-2d.png)

![Relocated 3D sample with imported Blender arch](collect-3d.png)
