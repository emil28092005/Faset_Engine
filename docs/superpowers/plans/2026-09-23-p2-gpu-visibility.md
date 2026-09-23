# P2 GPU Visibility and Mesh LOD Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deliver all of PLAN P2: stable GPU instance IDs, GPU frustum culling, fixed indirect bins, a visible HZB debug mode, conservative two-pass occlusion, prepared mesh LOD with hysteresis, and measured comparisons to the direct renderer.

**Architecture:** Keep the current direct path as a selectable reference. Add an opaque instanced path with local mesh geometry, instance/visible-ID storage buffers and compute-authored indirect instance counts. Split opaque rasterization around an ordinary-Z max-depth HZB; repair previous-frame occlusion guesses in a current-frame post pass. Keep shadows and ordered sprites/UI independent.

**Tech Stack:** C++20, Vulkan 1.3, Slang/SPIR-V, CMake/Ninja, SDL3, CPU contract tests and GPU validation/image tests.

**Spec:** `docs/superpowers/specs/2026-09-23-p2-gpu-visibility-design.md`

## Global Constraints

- Preserve the existing direct renderer and a runtime switch that selects it.
- Player/export contain cooked SPIR-V and metadata; no Slang compiler is required at runtime.
- MCP remains editor-only and never exposes the live Player world.
- Forward Z uses max-depth HZB, depth clear 1, and `LESS_OR_EQUAL`.
- No camera-frustum decision may remove a shadow caster; sprites and UI remain ordered.
- No required ray tracing, mesh shader, multi-draw-indirect, `drawIndirectFirstInstance`, or synchronous GPU visibility readback.
- Check actual Vulkan feature/format/limit support and retain a direct fallback if the P2 profile is unavailable.

## Review Focus

- Empty scene and exact bin capacity: indirect counts reset to zero and cannot address outside reserved ID ranges; Task 3 pins this.
- Door opens or disappears: a previously hidden object appears in the same final frame through Post; Task 5 pins this.
- Odd and resized viewports: HZB padding is far depth and stale history cannot be sampled; Tasks 4–5 pin this.
- Offscreen shadow caster: camera visibility leaves shadow generation intact; Task 3 pins this.
- Mesh/key reuse and LOD changes: previous transforms and occlusion are invalidated; Tasks 1 and 6 pin this.

---

**Implementation note, 23 September 2026:** this checklist records the proposed execution sequence. A checked item has direct code/test/commit evidence; an unchecked item may be an unrecorded test-first step or a narrower validation/documentation gap, even where the corresponding P2 feature works. The [acceptance protocol](../../studies/19-p2-gpu-visibility-acceptance.md), [initial benchmark](../../studies/20-p2-gpu-visibility-benchmark-2026-09-23.md) and [optimized repeat](../../studies/21-p2-gpu-visibility-optimization-2026-09-23.md) report the observed Linux scope. Other platforms and real-game performance require separate evidence.

### Task 1: Stable instances, conservative bounds, and LOD policy

**Files:** Modify `include/faset/render/renderer.hpp`, `src/player/SceneView.cpp`, `cmake/Renderer.cmake`; create `include/faset/render/visibility.hpp`, `src/render/visibility.cpp`, `tests/render_visibility_policy_tests.cpp`.

**Interfaces:** Produce `InstanceTracker::update(key, meshIdentity, model, bounds, viewId)` returning slot, generation, previous bounds and `previousValid`; `select_lod(projectedPixels, previousLevel, thresholds, hysteresis)`; `transformed_bounds(mesh, model)`. Add optional `DrawItem::instance_key`, `DrawItem::lod_meshes`, `Snapshot::view_id`, `Snapshot::camera_cut` without changing existing aggregate initialization order.

- [x] **Step 1: Write failing CPU tests.** Assert transformed bounds for rotated, negative/nonuniform scaled meshes; stable slot through reorder; generation change after removal/reuse or mesh replacement; no previous state on camera cut; LOD hysteresis on both sides of a threshold; fallback when a level is absent. Include this core assertion:
  ```cpp
  auto first = tracker.update("object/primitive", meshA, identity, box, "game");
  auto second = tracker.update("object/primitive", meshA, moved, box, "game");
  require(first.slot == second.slot && second.previous_valid);
  tracker.finish_frame();
  require(!tracker.update("object/primitive", meshB, moved, box, "game").previous_valid);
  ```
- [ ] **Step 2: Run the focused test target and record the expected missing-interface failure.** `cmake --build --preset linux-debug --target faset_render_visibility_policy_tests -j 6` must fail before implementation because the new interfaces/target do not exist.
- [x] **Step 3: Implement the policy and extraction keys.** Derive keys from persistent scene object and imported primitive identity; anonymous draws remain renderable without temporal state. Use all eight local AABB corners and finite-value checks. Define an explicit LOD threshold/hysteresis contract in the header.
- [x] **Step 4: Rebuild/run the focused CPU test, then the existing scene-view tests.** `ctest --test-dir build/linux-debug --output-on-failure -R 'visibility_policy|player_scene_contracts'` must pass.
- [x] **Step 5: Commit** `Introduce stable render instances and prepared LOD policy`.

### Task 2: P2 Slang bundle and checked shader metadata

**Files:** Create `shaders/gpu_scene.slang`; modify `tools/compile_shader.py`, `cmake/Renderer.cmake`, `src/render/shader_contract.cpp`, `src/render/shader_contract.hpp`, `src/editor/build_service.cpp`, relevant shader/build tests.

**Interfaces:** Compile and validate `gpuVertexMain`, `gpuShadowMain`, `gpuCullMain`, `gpuHzbMain`, `gpuPostCullMain` with documented set/binding layouts. Baseline `vertexMain`, `fragmentMain`, `shadowMain` and their hot-reload fingerprint contract remain valid. Export includes each new `.spv` and `.reflection.json` pair.

- [x] **Step 1: Write failing reflection/package tests.** A storage-buffer/storage-image Slang reflection fixture must normalize to a typed descriptor; missing or tampered P2 SPIR-V/metadata must fail validation; a packaged Player must contain all required P2 shaders.
- [ ] **Step 2: Run focused tests and verify the expected rejection or missing-artifact failure.** `ctest --test-dir build/linux-debug --output-on-failure -R 'render_shader_reload|build_schema_publication'` plus the new reflection test target.
- [x] **Step 3: Extend the compiler's descriptor normalization and add the P2 shader entries.** Vertex resolves `visibleIds[binBase + SV_InstanceID]`; compute writes bounded per-bin IDs/counts; HZB computes max of valid children and far depth for padding. Keep C++/Slang record strides explicit and checked.
- [x] **Step 4: Validate generated reflection and package.** Rebuild `faset_shaders`, run the focused tests and inspect each generated metadata stage/binding/fingerprint.
- [x] **Step 5: Commit** `Add checked Slang shaders for GPU visibility and HZB`.

### Task 3: GPU frustum culling and fixed indirect bins

**Files:** Modify `src/render/renderer.cpp`, `include/faset/render/renderer.hpp`, `tests/render_tests.cpp`; create `tests/render_gpu_visibility_tests.cpp`.

**Interfaces:** `RendererConfig::visibility_mode` selects direct/auto/GPU; `FrameStats` exposes submitted bin and frustum-visible counts. Renderer-owned buffers hold local vertices, instance records, candidate/bin tables, visible IDs and `VkDrawIndirectCommand` templates. One draw per bin uses `firstInstance=0`.

- [ ] **Step 1: Write GPU integration tests.** Direct/GPU images must agree on the same opaque cube/plane scene; empty and one-instance scenes have zero/one visible instances; over-capacity growth and all-six-plane rejects remain validation-clean; an offscreen caster continues to affect a visible receiver.
- [ ] **Step 2: Run the new GPU test and verify that GPU mode is absent/fails for the intended reason.** Use `ctest --test-dir build/linux-debug --output-on-failure -R '^render_gpu_visibility$'`.
- [x] **Step 3: Add checked device capability selection and distinct GPU scene resources.** Build mesh+texture bins, upload local vertices once per unique mesh each frame, reserve one ID range per bin, create/update descriptors, dispatch frustum cull, barrier compute writes to indirect and vertex-storage reads, and issue fixed indirect draws. Keep the direct path byte-for-byte selectable.
- [x] **Step 4: Run the focused GPU test with Khronos validation and the existing offscreen renderer tests.** No Vulkan errors, no visible image holes, and no shadow regression.
- [x] **Step 5: Commit** `Render opaque meshes through GPU culling and fixed indirect bins`.

### Task 4: Current HZB and diagnostic view

**Files:** Modify `src/render/renderer.cpp`, `include/faset/render/renderer.hpp`, `shaders/gpu_scene.slang`, `tests/render_gpu_visibility_tests.cpp`; add a focused HZB CPU oracle test if the reduction/projection contract needs isolation.

**Interfaces:** `RendererConfig` or `Snapshot` selects HZB visualization; `FrameStats` records HZB extent, levels and build time. The current depth attachment is stored and sampled; a per-mip `R32_SFLOAT` image stores max-depth reduction.

- [ ] **Step 1: Write failing tests.** Assert odd 319×241 extent, a far-depth hole, mip chain dimensions/padding, resized target recreation and an HZB debug image with non-uniform depth.
- [ ] **Step 2: Run the tests and confirm the missing HZB/debug capability is the failure.** `ctest --test-dir build/linux-debug --output-on-failure -R '^render_gpu_visibility$'`.
- [x] **Step 3: Split Main opaque raster from sprites/UI, store depth, allocate sampled/storage HZB mip views, dispatch each reduction with explicit depth→compute and mip→mip barriers, and draw a selectable debug visualization.** Preserve existing forward-Z convention.
- [x] **Step 4: Run focused GPU tests and baseline image tests under validation; compare HZB-off output with direct mode.**
- [x] **Step 5: Commit** `Build and visualize current-frame max-depth HZB`.

### Task 5: Previous-HZB main pass and same-frame post repair

**Files:** Modify `src/render/renderer.cpp`, `src/render/visibility.cpp`, `shaders/gpu_scene.slang`, `tests/render_gpu_visibility_tests.cpp`.

**Interfaces:** View history contains view identity, extent/viewport/projection, frame epoch, previous VP and HZB handle. MainCull may defer only a candidate with valid previous instance/view history; PostCull retests deferred candidates against current HZB and writes separate post args/IDs. PostRaster loads existing color and depth.

- [ ] **Step 1: Write failing frame-sequence tests.** A wall hides an object in frame N; opening/deleting/teleporting it in N+1 reveals that object in the final N+1 image. Camera cut, projection change, view ID change and resize force history invalid; near-plane crossing fails open. Compare every frame against HZB-disabled output.
- [ ] **Step 2: Run the tests and observe the missing deferral/post behavior.** `ctest --test-dir build/linux-debug --output-on-failure -R '^render_gpu_visibility$'`.
- [x] **Step 3: Implement previous/current projection tests, bounded deferred/post buffers, history ping-pong and invalidation, explicit compute→indirect/vertex barriers and Main/CurrentHZB/Post pass order.** Use ordinary-Z max-depth comparison with precision bias and full projected rectangle.
- [x] **Step 4: Run the sequence and full GPU render suites with validation; confirm no same-frame holes.**
- [x] **Step 5: Commit** `Repair temporal occlusion with current-frame post pass`.

### Task 6: Prepared mesh LOD, editor controls and profiling

**Files:** Modify `src/player/SceneView.cpp`, `src/editor/editor_ui.cpp`, `include/faset/render/renderer.hpp`, `src/render/renderer.cpp`, `tests/render_gpu_visibility_tests.cpp`; add a prepared-LOD example asset/scene and manual page under `docs/manual/editor/`.

**Interfaces:** Imported or C++-supplied prepared LOD meshes are selected by projected size and Task 1 hysteresis. Changing level changes the bin but keeps the logical instance key and invalidates previous occlusion. Editor can switch direct/GPU/HZB modes and inspect culling/LOD counters without adding MCP access to the Player.

- [x] **Step 1: Write failing tests.** Jitter around both thresholds must retain the previous LOD; moving well across a threshold selects a different mesh/bin; missing levels fall back; both modes keep a valid image through level changes.
- [ ] **Step 2: Run focused tests and verify the missing selection/control behavior.**
- [ ] **Step 3: Hook prepared LODs into extraction and GPU binning, expose compact editor controls/statistics and a documented C++/asset authoring path.** Keep source scene IDs unchanged.
- [x] **Step 4: Run CPU/GPU/editor UI suites and capture a representative debug screenshot.**
- [x] **Step 5: Commit** `Expose prepared mesh LOD and GPU visibility diagnostics`.

### Task 7: Adversarial validation, baselines and publication

**Files:** Add `docs/validation/p2-gpu-visibility/README.md` and capture artifacts; update `PLAN.md`, `docs/manual/editor/profiling.md`, `docs/ARCHITECTURE.md`, CI GPU test registration as appropriate.

**Interfaces:** Close P2 only with exact revision, compiler/driver/GPU/OS, scene/camera paths, commands, full-frame direct/GPU timings, pass counters, image comparison and stated coverage limits.

- [ ] **Step 1: Add adversarial fixtures for mass deletion/reuse, near-plane, camera inside bounds, odd/offset viewport, door/wall, open scene and offscreen shadow caster; confirm at least one fails before its corresponding fix.**
- [ ] **Step 2: Run Debug and Release build/test suites, GPU validation, shader reload, two sample-game export/relaunch checks, and available Windows CI.** Record exact outputs; a platform without executed GPU coverage remains explicitly unverified.
- [ ] **Step 3: Profile the same closed and open scenes in direct and GPU modes.** Record CPU extraction/upload/submission, GPU pass and full-frame time, readback conditions, memory and culling counters. Do not turn a scene-specific result into a universal performance claim.
- [ ] **Step 4: Review all PLAN P2 criteria against evidence, update docs, run `graphify update .`, request independent code review and fix load-bearing findings.**
- [ ] **Step 5: Commit** `Validate and document P2 GPU visibility milestone`; publish only after all checks are green.
