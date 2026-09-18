# MVP acceptance dossier

Accepted engine source: **`4cb82556de31268d2bde73948dd1ff1b6c02f162`**. Recorded 18 September 2026. Release tag: **`v0.1.0-mvp`**, including the final documentation/evidence commit.

**Status: the C++ MVP is accepted for the recorded Linux and Windows profiles.** This dossier maps [PLAN M0–M9](../../PLAN.md) to implemented behavior and bounded evidence. The final publication changes documentation and the research viewer, not the tested engine/gameplay code. Test names below are CTest names, not claims of additional runs. Acceptance does not certify untested devices or turn the compatibility coverage limits below into passing checks.

## Evidence and revision boundaries

- **Final candidate Linux:** the integrated suite has **34 passed, 1 skipped, 0 failed** out of 35 tests, with optional ImGui diagnostics enabled. The skip is native Wayland programmatic restore. Both exact-candidate Release games also passed standalone relocation/validation and 120 frames each. See the [final Linux record](final-linux-2026-09-18/README.md) and [test outcomes](final-linux-2026-09-18/tests.json). The earlier XWayland lifecycle scenario is separate.
- **Final candidate native/manual CI:** [run 35301244366](https://github.com/emil28092005/Faset_Engine/actions/runs/35301244366) passed. This is separate from the full Windows graphics/export workflow.
- **Sanitizers:** the [recorded configurations](final-linux-2026-09-18/tests.json) show complete `0f34b03` ASan/UBSan **18/18** and final-candidate asset/cook **2/2** after the source-relocation correction. This is not a claim that the complete sanitizer suite was repeated on `4cb8255`.
- **Exact earlier Linux candidate:** [checkpoint 5 evidence](checkpoint5-linux-2026-09-18/README.md) records clean offline build, first project launch, standalone games, recovery and real Blender reimport at `0f34b036313c011861dbfd5828ed45c4f7940b05`. The Blender report separately identifies the updated verification harness. These reports retain their original provenance.
- **Final source Windows:** [run 35301244334](https://github.com/emil28092005/Faset_Engine/actions/runs/35301244334) passed the fresh Editor/Player build, all **35 tests with no skips**, real BuildService Release exports/incremental rebuilding, and both checked-in games after standalone Unicode relocation. The [Windows evidence](windows-software-vulkan-2026-09-18/README.md) separates this exact `4cb8255` result from the preceding successful `0f34b03` run. SwiftShader is software Vulkan; the Khronos validation layer was unavailable.

## Criterion map

### M0 — reproducible foundation

CMake separates Core, Runtime, Editor, Player and SchemaExporter; gameplay is static, and Player does not link Editor/MCP/import services. Presets, compiler/SDK/runtime requirements, dependency hashes, Slang and third-party notices are recorded in [toolchains](../TOOLCHAINS.md), [dependency pins](../../dependencies.lock.json) and [target definitions](../../cmake/Player.cmake). Core tests cover IDs, hashing, atomic IO, Unicode paths and subprocess behavior.

Evidence: `core`, `process_and_cook`, native CI, and the [clean offline build](checkpoint5-linux-2026-09-18/offline.json): committed `0f34b03`, empty build directory, external network disabled, prepared tools/dependencies, 20 CPU tests. A prepared offline build does not establish a fresh installation of every system prerequisite. Windows records the clean checkout, pinned tools, configure/build commands and native test execution separately.

### M1 — documents, metadata and commands

Persistent source IDs are separate from runtime handles. Typed metadata describes stable fields, constraints and versions; unknown data survives round-trip. Authoring transactions validate candidates before publication and share revision conflicts, retry keys, Undo/Redo, save and recovery. Explicit declarative migrations preserve IDs and unknown fields; missing/manual/incompatible rules fail without partial edits.

Evidence: `authoring`, `build_schema_publication`, `editor_session_settings`, `editor_mcp`, and migration actions in `editor_ui_authoring`; [authoring fixtures](../../tests/authoring_tests.cpp) and [metadata publication fixtures](../../tests/build_schema_tests.cpp). Opening older data does not migrate it automatically. Changing field units requires reviewing separately stored instance overrides, as described in the [schema API guide](../manual/scripting/api.md).

### M2 — platform and baseline graphics

SDL window/input/text/DPI feeds a direct Vulkan 1.3 backend with capability checks, explicit synchronization and resource lifetime handling. The serial Render Graph declares reads/writes, barriers and optional GPU labels. Pinned Slang produces SPIR-V and normalized reflection; incompatible or failed shader reload preserves the working pipeline. Baseline rendering includes sprite transparency/layers, static textured meshes, basic PBR, directional shadows, CPU culling and timing/resource counters.

Evidence: `render_graph`, `render_offscreen`, `render_sprite_alpha`, `render_shader_reload`, `render_window_lifecycle`, and `editor_debug_overlay`; [shader reload regression](../../tests/render_reload_tests.cpp) and [native window evidence](native-window-linux-2026-09-18.json). Shipping packages contain SPIR-V rather than requiring Slang. Native Wayland restore and physical-device limits are stated below.

### M3 — runtime, C++, physics and Player

EnTT-backed runtime APIs preserve source identity and validate handles. Deferred structural changes, input/fixed/physics/event phases, bounded catch-up, interpolation and lifecycle callbacks have contract tests. Box2D/Box3D support the demo bodies, layers/events, grounding and debug shapes. A separate statically linked Player receives an immutable resolved authoring snapshot, including unsaved edits; Play/Stop/pause/step do not write simulation state into the document. Failed C++ or metadata publication keeps the previous build and reports stale status.

Evidence: `runtime_contracts`, `player_scene_contracts`, `player_shutdown_diagnostics`, four `tutorial_*` tests, `playable_2d`, `playable_3d`, `build_schema_publication`, and the [seven recovery scenarios](checkpoint5-linux-2026-09-18/recovery.json). The supported physics profile remains root-level box bodies, not arbitrary mesh-collider cooking.

### M4 — retained editor UI

The main UI uses retained IDs, layout/styles, clipping/scroll, keyboard focus, editing/selection, drag/drop, one-window docking and dynamic metadata fields. One gesture creates one authoring Undo. FreeType/HarfBuzz and UTF-8 text support Cyrillic; focus reveals long/nested lists at 1×/2×. Layout/style reload validates candidates and preserves working state. Optional F12 ImGui diagnostics displays real renderer counters without replacing the main UI.

Evidence: `ui_widgets`, `ui_render`, `editor_ui_reload`, `editor_ui_gizmos`, `editor_ui_authoring`, `editor_debug_overlay`; [widget regression](../../tests/ui_tests.cpp) covers composition events, clipboard callbacks, DPI rasterization/hit testing and scroll/focus behavior. These deterministic events do **not** establish physical OS IME or mixed-monitor behavior.

### M5 — resources and Blender

The importer owns cooked geometry/material/texture data, persists source identity/settings, hashes inputs/dependencies/toolchain/profile and publishes complete generations atomically. Ordinary GLB and PNG/JPEG work without Blender. Freshness is visible through UI/MCP; stale sources cannot silently pass cook/export. Removal review pins both candidate and active generations; failed/cancelled imports retain the last good result. The final relocation correction updates logical source and payload together while keeping immutable generations and old pointers readable.

Evidence: `assets_pipeline`, `assets_blender_bundle`, `editor_ui_import_conflicts`; [asset fixtures](../../tests/assets_pipeline.cpp) cover cache rebuild, stable/unstable rename, removal, failure/cancel, freshness, moved PNG/bundle/external glTF and legacy pointers. The [real Blender report](checkpoint5-linux-2026-09-18/blender.json) shows unmodified Blender 4.5.3 updating two live Editor instances while preserving placement, tint, physics, opaque gameplay and revision. It used `0f34b03` binaries; relocation coverage is from the subsequent final-candidate tests. The [documented import profile](../manual/editor/assets.md) limits material/geometry transfer.

### M6 — scene editing and reusable scenes

Scene Tree, Inspector, Assets, viewport selection/gizmos, Console, project/simulation settings and Save/Play/Build are real authoring controls. Templates use stable nested instance/object/component addresses, sparse overrides, local additions/suppression and restricted local/world reparenting. Origin, Revert, source navigation and conflicts are visible. Schema changes invalidate resolved previews even without a document revision change.

Evidence: `editor_ui_launcher`, `editor_ui_project_settings`, `editor_ui_templates`, `editor_ui_gizmos`, `editor_ui_authoring`. The [template workflow](../../tests/editor_ui_templates.cpp) saves through UI, opens a new Session and compares nested IDs, origins and values. Cache clearing/reimport is covered separately by `assets_pipeline`. [First launch](checkpoint5-linux-2026-09-18/first-run.json) follows the Manual on the clean `0f34b03` Linux build. The [Windows record](windows-software-vulkan-2026-09-18/README.md) ties its fresh source build to actual launcher Create/Open and native Editor/MCP tests; it does not claim a retail installer or a human usability study.

### M7 — editor MCP and extensions

GUI and MCP share commands, canonical documents, revisions and history. Headless authoring/build, structured errors and cancellable jobs are separate from GPU screenshots. MCP is confined to editor services; Player and SchemaExporter contain no runtime-world MCP API. Startup DLL/SO extensions use exact SDK/build compatibility, dependencies and registration ownership.

Evidence: `editor_mcp`, `editor_mcp_stdio`, `editor_gui_mcp`, `editor_plugins`; [GUI/stdio regression](../../tests/editor_gui_mcp_test.py) covers actual captures and conflicting edits/Undo; [plugin fixtures](../../tests/plugin_tests.cpp) cover the runtime-component/editor-panel package, compatibility rejection and unknown-data preservation. This is the supported protocol/workflow scope, not certification of every MCP client.

### M8 — standalone games and export

BuildService validates metadata, builds C++, cooks resources/shaders, assembles a manifest/notices and validates the standalone Player before publishing. Editor/MCP/Blender/schema helpers/compiler are not required by the exported game. Both checked-in C++ games exercise controls, physics, pickups, a gate/exit and reset; the 3D game uses the Blender arch.

Evidence: `process_and_cook`, `build_schema_publication`, `playable_2d`, `playable_3d`, shader/template/Blender regressions, and [exact `4cb8255` Linux exports](final-linux-2026-09-18/playable-exports.json). Both packages ran 120 frames after Unicode-path relocation with source projects hidden and zero errors under active Khronos validation. [2D](final-linux-2026-09-18/collect-2d-manifest.json) and [3D](final-linux-2026-09-18/collect-3d-manifest.json) manifests retain package hashes. An additional [Linux namespace check](final-linux-2026-09-18/sdk-unavailable.json) hid the entire SDK/build/tool tree and repeated validation/120 frames with identical captures. [Final Windows exports](windows-software-vulkan-2026-09-18/README.md) passed the same project relocation checks on SwiftShader. **All four dimension/platform combinations passed.** The entire-SDK hiding check was Linux-only; it is not silently attributed to Windows.

### M9 — final acceptance

Existing evidence covers shared GUI/MCP authoring, revision conflicts, Undo/Redo, interrupted Editor recovery and failed save/import/build/Player startup. The [Release baseline](linux-release-2026-09-18/README.md) records scene/hardware/settings, startup and CPU/GPU/readback/memory data; the [implementation log](../IMPLEMENTATION.md) records build/import iteration measurements. Its scoped thresholds serve as initial P1 tracking budgets for the recorded scenes/reference host, not engine-wide guarantees or enforced MVP performance gates. Those measurements retain their original revision and workload limits and are not a benchmark of the final candidate or every platform.

The native build/export gates, first-project workflows and recovery checks are complete for the profiles below. PLAN and the Manual record current capabilities, with compatibility coverage limits retained explicitly. The first release is a source MVP for these small projects, not a claim of production readiness for arbitrary games.

## Final disposition

- **LINUX-EXPORT-4CB — passed:** [final report](final-linux-2026-09-18/README.md), source clean at `4cb8255` through completion; both Release packages validated and rendered 120 frames after Unicode relocation with source paths unavailable. RTX 2080 Ti, Khronos validation active, zero errors. Package hashes and per-game counters are linked from that record; the earlier `0f34b03` reports retain their provenance.
- **WINDOWS-TEST-4CB — passed:** [run 35301244334](https://github.com/emil28092005/Faset_Engine/actions/runs/35301244334), fresh build with optional diagnostics, 35/35 tests, no failures/skips. Exact outcomes and native UI/window observations are retained in the [Windows dossier](windows-software-vulkan-2026-09-18/README.md).
- **WINDOWS-EXPORT-4CB — passed:** both Release games, package hash checks, Unicode relocation, disposable source projects unavailable, 120 rendered frames each. Required system/runtime libraries and the software Vulkan driver remain part of the target environment. No physical Windows GPU or active Khronos layer is claimed.
- **FIRST-PROJECT — passed within the developer setup profile:** clean Linux SDK plus the Manual's create-project command; fresh Windows checkout/toolchain configure/build followed by actual Create/Open, native window and GUI/MCP execution. System prerequisites were prepared on Linux or provided/prepared by the Windows runner. No graphical retail installer was planned or tested.
- **RELEASE — `v0.1.0-mvp`:** M0–M9 are closed for the documented profiles. Remaining native IME, mixed-monitor, native Wayland restore and additional hardware-driver coverage stay visible below; Lua and subsequent feature work remain in P1–P6. The UI references guide appearance only; architecture, PLAN and working authoring contracts define behavior.

## Coverage limits that must remain visible

- **Physical OS IME:** synthetic preedit/commit and SDL input-area checks passed; an actual system IME/candidate window/composition workflow was not exercised. Clipboard checks do not substitute for it.
- **Physical mixed-monitor DPI:** tests cover 1×/2× layout, glyph density, input coordinates, focus and live-scale state preservation; moving a native window between actual monitors with different scales was not tested.
- **Native Wayland restore:** the compositor declined the programmatic restore scenario, which is explicitly skipped. XWayland passing is useful alternative-backend evidence, not native Wayland completion.
- **Platform and performance scope:** Linux hardware results do not certify physical Windows GPUs. Software-driver functional coverage does not establish hardware performance. Small fixed scenes and static frame profiles do not establish large-project responsiveness.

These are verification limits, not proof that the feature is broken; they also cannot be silently counted as verified. The supported MVP profile remains static glTF/UV0/basic PBR, root-level box physics, one editor window and C++ gameplay. Lua, advanced rendering/animation, live link, variants and C++ hot replacement remain outside this acceptance scope as specified by PLAN.
