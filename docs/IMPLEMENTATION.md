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
