# Implementation checkpoints

This log records working implementation and observed validation. It does not replace
the acceptance criteria in `PLAN.md`. Incomplete platform or workflow checks remain open.

## Checkpoint 1 — native foundation and independent subsystems

Implemented:

- C++20 CMake presets, pinned dependency archives with SHA-256 verification, and separate targets.
- Core persistent IDs, SHA-256, atomic file replacement, JSON IO, and project path boundaries.
- Explicit schema registration, document transactions, revisions, retry keys, Undo/Redo,
  recovery records, unknown extension preservation, migrations, and nested template resolution.
- EnTT runtime with real Box2D/Box3D, fixed ticks, deferred changes, checked handles,
  interpolation, and statically linked example behaviors.
- GLB/glTF asset import, cooked mesh data, stable source identity, cache generations,
  reimport conflicts, cancellation, and the optional Blender export add-on.
- Direct Vulkan renderer, Slang shaders, sprites/meshes, basic PBR and directional
  shadows, texture upload, resize, capture, and SDL input.
- English MkDocs/Material manual and an image prototype for the future retained editor UI.

Observed validation on Linux:

- Integrated headless CTest: authoring, runtime, asset pipeline, Blender bundle, core — 5/5 passed.
- Integrated Clang 21.1.8/Ninja build with Vulkan: 7/7 tests passed, including offscreen GPU rendering.
- Independent runtime and asset AddressSanitizer/UndefinedBehaviorSanitizer checks passed.
- Renderer offscreen/visible tests exercised NVIDIA RTX 2080 Ti with Vulkan validation.
- MkDocs strict build passed with MkDocs 1.6.1 and Material 9.7.7.

The full editor, user-project build pipeline, MCP integration, standalone exports,
and Windows acceptance were still being implemented at that checkpoint. Later progress is recorded below.

Known intermediate constraints include box-only physics colliders, root-level physics
objects, static glTF triangles/UV0, a conservative serial renderer, and then-unfinished
world-preserving authoring reparent operations (completed in checkpoint 2). These remain implementation work or
explicit profile limits to review during final acceptance.

## Checkpoint 2 — integrated Editor, gameplay iteration and export

Implemented and integrated:

- Separate Player and SchemaExporter executables; statically linked project gameplay,
  configurable simulation settings, real contact-based grounded queries, and four
  compiled scripting tutorials embedded directly into the English MkDocs manual.
- Shared authoring commands and a real stdio MCP server, optimistic revisions,
  transactional retries, jobs/cancellation, recovery of unsaved documents, and
  viewport capture in graphical sessions only.
- Cancellable native subprocess execution, incremental project builds, schema export,
  binary scene/resource packaging, distinct Debug development and Release export
  directories, immutable published generations and retained last-good builds.
- A native retained UI with pinned FreeType/HarfBuzz and bundled Noto Sans:
  scene tree, schema Inspector, viewport camera/picking/gizmos, assets, diagnostics,
  build/play controls, commands, recovery and startup-loaded extension action panels.
- Exact-build native Editor SDK, dependency validation, owner-bound registrations,
  example Beacon runtime component/editor command/panel, and unknown-data preservation.
- Full world-preserving TRS reparent with explicit rejection of unsupported shear.
- Read-only cooked asset target for Player. Import/build/editor/MCP services remain
  outside the shipping runtime dependency graph.

Observed validation:

- Integrated Linux Clang 21 build: 19 CTests pass, including GPU rendering, retained
  widgets, Editor authoring interaction, actual plugin loading, real MCP stdio,
  process/cook contracts, and all four compiled gameplay tutorials.
- Real 2D and imported-glTF 3D standalone exports render with zero Vulkan validation
  errors on NVIDIA RTX 2080 Ti. Changing gameplay rebuilds metadata; failed C++
  compilation preserves the last successful published build.
- Runtime/tutorial checks also pass with Clang 18; runtime ASan/UBSan checks pass.
- Unmodified Blender 4.5.3 exports through the optional add-on. Real Editor imports
  preserve output IDs after rename/geometry edits, report deleted-output conflicts,
  retain the last generation on failure, and preserve separate scene placement/color.
  Reproduce with `tools/verify_blender_roundtrip.py --blender PATH --editor PATH`.
- Strict MkDocs build passes. Tutorial source snippets are compiled by CTest.
- Previous checkpoint headless Linux and Windows GitHub CI passed after portability
  fixes. New checkpoint and full Windows graphical/export checks are separate work;
  Linux validation does not imply Windows validation.

This is an implementation checkpoint, not an MVP release. Finished playable sample
projects, complete Windows graphical/export acceptance, fresh-install checks,
performance measurements, and final UX review remain. Standalone image import is
integrated but its dedicated PNG/JPEG edge-case checks are the next asset task.

The baseline profile currently uses box colliders, root-level rigid bodies, static
triangle glTF meshes/UV0, basic PBR/directional shadows and a conservative serial
Vulkan renderer. Advanced rendering and broader content profiles remain later work.
The generated UI reference determines visual direction only; architecture, behavior
and acceptance criteria remain authoritative.

## Checkpoint 3 — playable projects and complete authoring workflows

Implemented and exercised on Linux:

- Two playable C++ projects with real Box2D/Box3D input routes, pickups, a physical
  gate, an exit condition and reset. The 3D project includes a reproducible original
  Blender arch, `.blend` source, stable-ID bundle and import instructions.
- Native project launcher, retained folder browser, real recent projects, keyboard
  navigation and safe project switching. GUI/MCP sessions retain one fixed project.
- Nested template UI, source navigation, local additions/suppression/reparent,
  per-field origin/Revert and conflict preservation. Parented gizmos have tested
  transforms, cancellation and one committed Undo operation.
- Explicit Project settings with file-content revisions and atomic save; per-scene
  Simulation remains a separate authoring transaction. Live theme/layout reload
  validates candidates and keeps working state on malformed edits.
- Standalone PNG/JPEG assets; versioned material records and explicit portable
  cache profile/toolchain identity. Clearing cache preserves identity and overrides
  when original sources and sidecars are reimported.
- Normalized Slang reflection, shader artifact hashes and renderer ABI compatibility
  validation. Real compile failure, changed binding/matrix layout and invalid SPIR-V
  keep a working pipeline; compatible pixel-changing reload succeeds.
- Physics debug box outlines, bounded raw Player profiles, observed validation
  activation, CPU/GPU/readback durations and allocation counters.
- MCP broken-pipe/EOF handling and fresh GUI capture after presentation back-pressure.
  A real GUI + stdio regression performs 12 PNG captures, a conflicting edit and Undo.
- English manual guides for workspace, templates, Blender, export and profiling,
  alongside compiled C++ tutorials. Generated UI references remain visual guidance.

Acceptance evidence:

- The integrated Linux test suite passes **29/29 tests**, including ten GPU tests:
  native CPU, Vulkan, UI, real MCP transport, plugins, schema, physics and playable
  input routes. Strict MkDocs also passes.
- `tools/verify_playable_exports.py` exported the exact 2D/3D samples in Release,
  imported the Blender arch, checked package hashes and relocated each package outside
  its project. With the source-project paths unavailable, both passed validation and
  120 offscreen frames on RTX 2080 Ti with the Khronos layer active and zero errors.
- Renderer/shader regressions also passed the pinned Linux SwiftShader driver. This
  is additional software-driver coverage, separate from Windows execution.
- `tools/measure_workflows.py` recorded a fresh sample Debug build and incremental
  iteration, including stale-schema transitions. On the development host, the
  initial build took 93.06 s, unchanged build 5.59 s, changed gameplay build 11.18 s,
  and the subsequent one-frame Player process 0.37 s. The first/cached small Blender
  arch imports each took about 0.047 s including Editor startup. Ambient builds were
  running; these are observations, not release budgets.
- Profiling identified uncached readback memory as a concrete bottleneck. In a small
  paired five-frame diagnostic, preferring compatible HOST_CACHED memory reduced
  median readback from 27.87 to 0.47 ms and renderer-call wall time from 30.81 to
  2.92 ms. GPU work was about 0.58 ms. A longer final baseline is still required.

This remains an implementation checkpoint. Windows full graphics/export CI is still
building its pinned software driver. A review also identified Windows Unicode path
boundaries that must be corrected before cross-platform acceptance. Clean offline
build verification, final performance baselines and the final acceptance record
remain open; no MVP tag has been created.

### Checks after checkpoint 3

- Public commit: `d834cfad67cd81d8c4998b90c16791361ca8c0f8`.
- Linux and Windows headless CI plus strict manual passed in GitHub run
  [35295534027](https://github.com/emil28092005/Faset_Engine/actions/runs/35295534027).
- A clean committed source snapshot built all native targets with external networking
  disabled; all 19 CPU tests passed. The recorded build/test phase took 174.03 s.
  [Offline evidence](validation/offline-linux-2026-09-18.json) distinguishes prepared
  system tools from source/dependency inputs. GPU/window checks remain separate.
- The first full Windows graphics job compiled the whole Editor successfully, then
  failed creating a Vulkan instance with its CI software driver. Its CPU SceneView
  test also reported a texture-loading error. These are acceptance failures under
  investigation, not verified Windows graphics/export support.

## Checkpoint 4 — portability and acceptance hardening

Implemented after concrete regression findings:

- UTF-8 project, resource, plugin, CLI and subprocess boundaries. Windows native paths
  stay wide internally; Editor/Player/SchemaExporter normalize `wmain` arguments.
  File reads, hashes and atomic writes use extended native paths. All Faset Windows
  executables declare long-path support; CI enables the documented developer policy.
- Normal Stop requests Player shutdown before bounded termination fallback. Real
  `onDestroy` execution was observed, and a dedicated Player fixture verifies that
  callback diagnostics also reach logs during shutdown. Custom component versions
  are checked against the linked gameplay schema, including data-only components.
- POSIX subprocess cleanup keeps the exited leader's PID reserved while terminating
  its owned process group, then reaps it. A regression verifies that a descendant
  ignoring SIGTERM cannot survive normal leader exit or cancellation, without losing
  the leader's stdout tail or signaling a reused process-group ID.
- Transparent sprites use stable layer/depth ordering without depth writes, avoiding
  invisible sprites hiding objects behind them. GPU regressions cover alpha layering,
  shader/capture paths longer than 300 characters, and window capture after resize.
- Editor/launcher DPI scaling, logical-unit layout persistence and safe cancellation
  of unfinished drags on scale changes. Retained UI tests cover 1×/2× text rasterization,
  clipping, hit testing and text-input rectangles.
- Manual import-removal review with exact IDs/names and explicit acceptance. Both the
  reviewed candidate and active generation are checked before publishing, so stale
  confirmation cannot apply a different import. The GUI regression exercises rejection,
  fresh review and successful acceptance through actual widget events.
- Extended template workflow now saves through UI controls and opens a new Session,
  comparing IDs, nested origins, local additions and resolved values.
- Blender verification now updates two instances in one live Editor/SceneView, preserves
  placement, tint, physics and opaque gameplay fields, and retains both visible meshes
  after a removal conflict. [Blender evidence](validation/blender-live-linux-2026-09-18.json).

Observed Linux integration: **32 passed, 1 skipped, 0 failed** in the 33-test suite.
The skip is native Wayland programmatic restore, which the compositor declined;
the same lifecycle scenario passed under XWayland. ASan/UBSan passed **17/17** tests.
Strict MkDocs passed. [Recovery evidence](validation/editor-recovery-linux-2026-09-18.json)
records actual Editor termination and failed save/import/build/Player scenarios.
[Release baseline](validation/linux-release-2026-09-18/README.md) records the two
relocated Unicode-path packages and 240-frame profiles with their scope and hashes.

Windows execution of these fixes remains a separate gate. The previous fast CPU run
isolated a 264-character cooked path failure; the new native-path fixes must pass on
Windows before that issue is considered closed there. Native OS IME composition and
moving between physical monitors still have less coverage than deterministic widget
and platform-boundary tests. No MVP tag has been created.

The final build-service review also reproduced a publication defect: an invalid
custom-field default can pass the build stage's shallow schema check before the
Editor rejects it. Full schema validation before publishing `last_build.json` is the
next bounded correction; checkpoint 4 does not claim this gate is already complete.


## Acceptance corrections after checkpoint 4

Gameplay schema publication now uses the same complete metadata validation as the
Editor, before either the Player generation or `last_build.json` is published.
A native fixture drives the real BuildService with valid custom schema v2 and twelve
invalid manifests, checking that the previous binary, schema, manifest and pointer
remain unchanged. The focused authoring/process/schema/Session/MCP suite passed 5/5.
An authoring-disabled Player configuration still builds without editor services.

Windows native checks passed for checkpoint 4 (`45bc352`) and `afd773f`, including
the previous long-path regression. The full graphics runner also passed its Vulkan
1.3 device probe after registering the pinned software ICD on the disposable elevated
runner. Full Windows graphics and export acceptance is still in progress; a successful
probe alone does not close that gate.

The final audit identified additional bounded work: source/dependency freshness in
the Assets panel, scrolling keyboard focus into view, delivery of explicit gameplay
schema migrations, GPU pass labels, and wiring optional ImGui diagnostics. These
remain under implementation and verification; the research map now links current
implementation evidence instead of claiming the engine has not been started.

## Checkpoint 5 — close the final authoring and diagnostics gaps

- Asset freshness compares source, bundle payload, external buffer/image, recipe,
  importer and profile contents without publishing a generation. GUI and MCP expose
  the same state/reasons; selecting a stale imported row prepares Reimport. Cook and
  export reject stale referenced sources while keeping the previous successful result.
- Gameplay schemas carry validated declarative migration steps. Inspector and MCP
  apply `component.migrate` as an explicit revision-checked Undo transaction. Opening
  old data remains possible without rules. Tests cover local components, instance-local
  additions, opening inherited sources, missing/manual rules and overflow rollback.
  The Manual explains the sparse-override limitation when field units change.
- Keyboard focus scrolls long and nested Inspector/Assets lists into view at 1×/2×,
  preserves unfinished text and keeps invalid numeric edits visible. Template preview
  and conflicts are also invalidated when schema metadata changes without a scene edit.
- Vulkan passes emit optional debug-utils labels. An optional `FASET_DEBUG_IMGUI=ON`
  Editor module shows real renderer diagnostics via F12; the Player remains independent.
  GPU tests cover textured/clipped ImGui geometry and event ownership for gestures
  crossing the panel in either direction. Offscreen clipboard operations are local
  to that renderer and never touch the desktop clipboard.
- The Windows launcher fixture compares canonical filesystem identities, including
  hosted-runner short TEMP aliases, and now distinguishes selection/path failures.

Integrated Linux with optional diagnostics enabled: **34 passed, 1 skipped, 0 failed**
of 35 tests. The skip remains native Wayland programmatic restore; XWayland passed.
ASan/UBSan passed **18/18**, including process cleanup, metadata publication and
migration transactions. Strict MkDocs and local Markdown file-link checks passed.

Windows run `35299805623` at `e0b9651` passed all **34** tests in its CPU/GPU/UI suite,
including the launcher fix, and proceeded to real native Release exports. That run
predates checkpoint 5's new authoring/diagnostic changes, whose Windows checks remain
separate. The next full Windows build enables the optional diagnostic module too.
No MVP tag is claimed at this checkpoint.
