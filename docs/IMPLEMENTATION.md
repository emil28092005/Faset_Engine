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
