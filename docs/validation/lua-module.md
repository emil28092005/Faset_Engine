# Lua module validation

Local implementation checks, 2026-09-18. These results supplement, not replace,
the earlier MVP acceptance record. Toolchain: Linux x86-64, GCC 13.3, CMake 4.4.3,
Ninja 1.13.2; pinned Lua 5.4.9.

## Observed results

| Configuration | Result |
|---|---|
| Lua enabled, renderer/editor UI disabled | 20/20 CTest tests passed |
| Lua disabled, renderer/editor UI disabled | 18/18 CTest tests passed |
| AddressSanitizer + UndefinedBehaviorSanitizer, Lua suites | 3/3 tests passed |
| Renderer-linked native Player and SchemaExporter | Built successfully; CPU Lua CLI contracts passed |
| Lua-only project without project C++ files | Empty native adapter built; sample validated; exactly two Lua schemas exported |
| Native Editor and Editor UI library | Compiled and linked; Editor `--help` ran |
| Manual | MkDocs strict build passed |

The Lua tests exercise lifecycle ordering, per-instance fields/state, VM ownership,
stale/cross-world handles, deferred structural operations, native physics contacts,
`require`, invalid schemas, CPU/memory limits and the shipped example scene. Additional
safety cases cover deep/cyclic JSON, repeated-string/key expansion, structural queue
limits, protected metatables, repeated OOM and reclamation of a failing instance.

BuildService tests exercise source snapshots, fingerprints, changes during a build,
Lua-only projects, export contents/notices, and switching back to Lua-free games.
Their native build/export fixture is a stand-in, not a graphical Player execution.

## Reproduce the CPU suite

```sh
cmake -S . -B build/lua-check -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DFASET_ENABLE_LUA=ON -DFASET_BUILD_RENDERER=OFF -DFASET_BUILD_EDITOR=OFF
cmake --build build/lua-check --parallel
ctest --test-dir build/lua-check --output-on-failure
```

Use a separate build directory with `-DFASET_ENABLE_LUA=OFF` for the optional-module
check. For sanitizers, configure with `-DFASET_SANITIZERS=ON`, build the
`faset_lua_tests`, `faset_lua_safety_tests`, and `faset_schema_exporter` targets, then
run `ctest --test-dir <build> --output-on-failure -R '^lua_'`.

## Not verified in the September 18 run

- Windows compilation or execution of the new module.
- Graphical/window interaction and real-time Lua reload in a running rendered game.
  `lua_player_reload` is provided as a GPU-labelled integration test for an equipped host.
- A complete real Release export launched on a separate machine.
- LeakSanitizer: this execution environment uses tracing incompatible with its
  process inspection, so sanitizer runs used `ASAN_OPTIONS=detect_leaks=0` and
  `UBSAN_OPTIONS=halt_on_error=1`. Address/undefined-behavior checks stayed enabled.

The renderer-linked CPU checks used the existing Vulkan loader, repo-pinned Vulkan
headers and cached Slang, with SDL X11/Wayland disabled. No system graphics packages
were installed. This proves linkage and CPU validation, not graphics compatibility.

## September 24 Release follow-up

The [Release execution record](lua-release-2026-09-24/README.md) closes the
Linux offscreen export gap above. At source commit `846f1f4`, a Release Editor
exported the checked-in Lua-only sample into a disposable Unicode-path project.
The package was relocated, the source project path was hidden, and its Player
validated and rendered 120 frames on an NVIDIA GeForce RTX 2080 Ti with driver
595.84 and Khronos validation enabled; zero validation errors were reported.
The package manifest has `lua_enabled: true`, declared Lua source and its license,
and no project C++ gameplay source. The five focused Release Lua/schema tests
passed. This verifies an offscreen run on Linux; a separate physical Windows GPU
and manual window interaction have not been tested by this record. Windows
SwiftShader execution is configured in CI and needs a passing run before it is
claimed as covered.

![Relocated Lua sample after 120 frames](lua-release-2026-09-24/lua.png)
