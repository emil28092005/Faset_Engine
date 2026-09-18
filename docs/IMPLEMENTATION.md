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
and Windows acceptance are still being implemented. This checkpoint is not the MVP release.

Known intermediate constraints include box-only physics colliders, root-level physics
objects, static glTF triangles/UV0, a conservative serial renderer, and unfinished
world-preserving authoring reparent operations. These remain implementation work or
explicit profile limits to review during final acceptance.
