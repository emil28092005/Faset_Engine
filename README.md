# Faset Engine

Faset is an independent engine project for desktop **2D and 3D games on Linux and Windows**. Its priorities are a custom editor that is comfortable to use by hand and through MCP, integration with Blender, and a path toward advanced graphics.

**Current status: MVP acceptance is in progress.** The native Editor, shared GUI/MCP authoring, C++ gameplay builds, Vulkan Player, standalone export and two playable sample games are integrated. Linux GPU workflows are tested; complete Windows graphics/export acceptance and final validation remain in progress. See the [implementation checkpoints](docs/IMPLEMENTATION.md) for observed results; a technology appearing in the plan does not mean it is complete or benchmarked.

## Start here

- [User manual](docs/manual/index.md) — learn C++ gameplay and follow working examples; maintained alongside implementation.
- [Development plan](PLAN.md) — milestones through MVP, acceptance criteria, and development beyond MVP.
- [Architecture](docs/ARCHITECTURE.md) — accepted decisions and subsystem boundaries.
- [Documentation](docs/README.md) — navigation and maintenance rules.
- [Source studies](docs/studies/README.md) — Unreal Engine, Godot, Unity, Blender, ECS, graphics, asset import, and builds.
- [Dependencies and independence](docs/DEPENDENCIES.md) — libraries, tools, and source provenance.
- [Toolchain profiles](docs/TOOLCHAINS.md) — recorded compilers, SDKs, graphics devices and offline preparation.

After following the manual's build setup, run `build/linux-debug/faset_editor`
(or `build/windows-debug/faset_editor.exe`) to open the project launcher. The
[`collect-2d`](examples/projects/collect-2d) and
[`collect-3d`](examples/projects/collect-3d) projects include playable C++ examples;
the 3D example includes an original Blender asset and import instructions.

## Language

English is the primary language of the engine: editor UI, built-in commands, diagnostics, public API identifiers, and CLI/MCP tool descriptions. Project content and user-entered text support Unicode; the engine's interface language does not dictate the language of a game.

This README is in English. The current planning documents, studies, and research map are primarily in Russian. Editor localization is a later milestone.

## Accepted foundation

- **C++** for the core and the first gameplay implementation. **Lua** will follow as a separate module and will be optional for individual games.
- Objects, components, and nested scene templates for authoring; **EnTT** for the runtime ECS. JSON authoring data, stable IDs, and cooked binary assets for export.
- A custom **Vulkan 1.3** backend, RenderGraph, and renderer. The backend calls Vulkan directly; gameplay uses Faset APIs. **Slang** compiles shaders, including compatible HLSL, to SPIR-V. The baseline renderer does not require ray tracing.
- **SDL3** behind Faset's platform API; **Box2D** and **Box3D** for physics.
- A custom retained-mode editor UI with C++ behavior, declarative layout, separate styles, and a dark theme by default. **Dear ImGui** is reserved for debugging tools.
- **CMake + Ninja + Clang**; Windows uses clang-cl, the Windows SDK, and MSVC runtime libraries. Initial builds are tested on their target OS.
- A separate **Player** process, statically linked with gameplay. The C++ iteration cycle is stop, incrementally rebuild, and restart.
- **Editor-only MCP:** authoring, assets, import, builds, export, Play/Stop, and editor diagnostics. MCP is absent from the Player and exported games.
- Standard, **unmodified Blender**, glTF/GLB import, and an optional add-on for convenient export and stable IDs.

MVP is complete when two small games, one 2D and one 3D, can be created, saved, played, and exported for both operating systems. GPU-driven rendering, HZB, advanced shadows, temporal reconstruction, and dynamic global illumination follow the baseline.

## Run the research map

Requires Node.js and npm. From the repository root:

```sh
cd docs/studies/map
npm ci
npm test
npm run build
npm start
```

Open [localhost:4178](http://localhost:4178). The map is a documentation viewer, not a web version of the Faset editor. See the [map README](docs/studies/map/README.md) for details.

## Repository contents

Faset's C++ source, tests, sample games, Blender add-on, Manual, architecture, studies,
selected validation evidence, and research map are tracked in Git. Third-party engine
source trees, installed dependencies, build outputs, and caches are excluded. Research
source links are pinned to the commits examined; Unreal Engine links may require
access through Epic.

See the [publication notes](docs/PUBLICATION.md) for publication scope and licensing status. A license for Faset's own content has not yet been selected. Third-party projects retain their own license terms.
