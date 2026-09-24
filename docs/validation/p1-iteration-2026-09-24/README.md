# P1 gameplay iteration: measured Linux workflow

The [unaltered version 2 workflow report](report.json) was produced by a clean
`6c2e8fa80036315a695f67f3c53524165fb4b10a` checkout on 2026-09-24.
It used disposable copies of the checked-in `collect-3d` and `examples/lua`
projects. Every sample, process log and Player profile is retained under `raw/`;
the report records the order and completion. The report identifies
each input revision by SHA-256, and records all command arguments, phase labels,
configuration and dirty-tree state. Its SHA-256 is
`fd463c0cd12eed0a22143aa68bd732386804b0df4ecb2a3d9caa26f59dcfe6e2`.

The original report's `toolchain.slang` is `null` because that version looked
for `slangc` on `PATH`, while CMake used a configured executable. A **separate**
[post-run provenance supplement](provenance.json), generated after commit
`abd8c21` from the surviving disposable [compressed CMake cache](CMakeCache.txt.gz), hashes
the selected CMake, Ninja, C/C++ compiler, Slang, Editor and UI-probe binaries.
It binds to the original report hash and records both script revisions. The
measured report was not rewritten. Subsequent workflow runs include these
identities in their report directly. `4ac02ee` changed only the Windows CI export
fixture location; neither follow-up is presented as the measured code revision.

## Measurement conditions and results

The host ran Linux 7.0, Ryzen 7 1700, 32 GiB RAM, RTX 2080 Ti and NVIDIA 595.84.
The disposable project used Debug, Clang 21.1.8, CMake 4.2.3, Ninja 1.13.2 and
Slang 2026.18. The Player rendered offscreen Vulkan at 1280×720 with validation
active and full-frame readback. The run did not overlap the separate P3 GPU sweep.
Build times still depend on ambient CPU load, filesystem cache and prepared
dependencies. The `cold` row means an empty project `.faset` cache, **not** a cold
OS page cache or a first dependency download.

| Observation | Samples | Median | Nearest-rank p95 | Scope |
| --- | ---: | ---: | ---: | --- |
| Initial configure and native build | 1 | 106.01 s | — | Disposable Debug project |
| Unchanged build | 5 | 11.53 s | 13.12 s | All five reused the verified schema/package generation |
| Changed `Gameplay.cpp` build | 5 | 28.16 s | 30.33 s | One warm-up edit excluded |
| Changed `Gameplay.hpp` build | 5 | 23.89 s | 25.51 s | One warm-up edit excluded |
| Watched Lua edit to reload log | 5 | 0.502 s | 0.504 s | Persistent Player; includes watcher polling |
| First completed offscreen Player frame | 5 | 0.284 s | 0.333 s | From `main()`; excludes OS loader and window presentation |
| Synthetic Editor input to rendered state | 100 | 13.97 ms | 18.03 ms | Ten warm-ups; offscreen retained-UI frame, not input-to-photon |

The intentional C++ compile failure took 10.00 s and the corrected recovery
build 19.93 s. The failed job preserved its prior successful build pointer.
The run also imported the small Blender bundle cold and cached in 49 and 52 ms,
respectively; Editor process startup dominates that tiny asset sample, so those
two values do not establish import-cache speedup. The raw
[failed-build job](raw/build_failure-01-stdout.txt) exposed a navigation defect:
the compiler named the staged source copy, and the original structured
diagnostic omitted `file`. A later fix maps only the verified staged `Scripts`
subtree to project-relative source. A [real failed-build replay](diagnostic-replay.json)
with the corrected Editor returns `Scripts/Gameplay.cpp`, line 73, column 2.
The `build_diagnostics` test also covers project-contained snapshot paths and
Windows drives; `build_schema_publication` verifies that an error after 250
third-party warnings survives the 200-row diagnostic limit. These are separate
from this unmodified measurement.

Follow-up `54d1bae` rehashes both the live source and the staged copy after
native build and before reuse/publication. A fixture changes the staged C++
header during schema export; the build fails and retains the last-good pointer.
The copy is content-verified at those boundaries, not sealed against a same-user
write-and-restore during compiler reads.

The [3,000-frame lifecycle profile](raw/cpp-3000-frame-lifecycle.json) completed
with zero Vulkan validation errors. Maximum explicit Vulkan allocation was
15,787,008 bytes in both frames 101–200 and 2901–3000, a measured growth of zero;
the live texture count remained one. These counters exclude driver-internal
memory and do not prove absence of all resource leaks.

The generated [527-entity scene](raw/generated-512.scene.json) adds 512 seeded
copies to the 15-entity source and ran for 240 offscreen frames with zero Vulkan
validation errors. Its [raw profile](raw/generated-512-profile.json) reports p95
frame wall 149.39 ms, simulation 48.19 ms, scene snapshot 68.12 ms, renderer call
36.51 ms and GPU timestamp 0.911 ms. It exposes a significant CPU scaling cost
in this synthetic content; it is not a typical-game benchmark or a Release budget
failure for the tiny checked-in sample.

## Reference budgets and coverage

The [2026-09-18 Release reference baseline](../linux-release-2026-09-18/README.md)
measured the **exact** `collect-2d` and `collect-3d` packaged scenes on this host,
at 1280×720 with validation/readback. Its 240-frame p95s met the initial tracking
budgets: frame 2.684/2.732 ms versus ≤4; GPU 0.579/0.600 ms versus ≤1; readback
0.633/0.608 ms versus ≤1; simulation 0.258/0.300 ms and snapshot 0.215/0.257 ms
versus ≤0.5 each. Explicit allocations were about 15.02/15.06 MiB versus
≤20 MiB; first-frame startup from `main()` was 265.5/260.8 ms versus ≤500 ms.
That baseline was an evolving earlier working tree. Its values are **historical**
and are not compared with this Debug workflow's timing.

The [fresh clean combined P1/P3 Release reference](final-release/README.md) at
`4a3453e` repeats these exact scenes on the same physical RTX 2080 Ti, now with
120-frame source-hidden relocation checks for C++ 2D/3D and Lua-only, plus
sequential 240-frame C++ 2D/3D profiles. The frame, GPU, readback, simulation,
snapshot and startup tracking budgets all pass. Explicit allocations instead
reach **43.022/43.118 MiB**, above the former ≤20 MiB budget because the new
renderer eagerly retains a 16 MiB sun atlas and a 16 MiB local atlas in both
scenes. The 2D sample casts no sun shadows and neither sample uses local shadow
faces. This is a measured tracking-budget miss, not a failed export or a reason
to redefine the counter silently. The linked dossier retains exact values,
atlas breakdown, raw profiles and source/toolchain provenance.

The [final integrated acceptance](../p1-p3-final-2026-09-24/README.md) reruns
all three relocated Release packages at clean `ed523c6` after the queued-edit
source-signature fix. A deterministic test first reproduced the false stale
schema state, then verified that Build and Export publish the worker's actual
source hash. This does not revise the older 240-frame timing or memory baseline.

The [Lua-only Linux Release dossier](../lua-release-2026-09-24/README.md) separately
records a relocated package that validated and rendered 120 frames after hiding
its source project, with 0 validation errors. The final fixture placement was
checked in clean-revision [`4ac02ee` Windows graphics CI](https://github.com/emil28092005/Faset_Engine/actions/runs/35937433040):
61/61 CTests passed on pinned SwiftShader, the real 2D/3D Release export
integration passed, and **all three** relocated Release games (`collect-2d`,
`collect-3d`, Lua-only) validated and rendered 120/120 frames with source paths
unavailable. The [Windows playable report](windows-4ac02ee/playable-report.json)
contains package and source hashes, device identity, result details and capture
checksums; [integration results](windows-4ac02ee/integration-result-2.json),
[integration manifests](windows-4ac02ee/integration-manifest-2.json),
[Vulkan probe](windows-4ac02ee/vulkan-probe.json) and
[toolchain record](windows-4ac02ee/toolchain.json) preserve representative raw
evidence. The parallel [native/manual CI](https://github.com/emil28092005/Faset_Engine/actions/runs/35937433211)
passed on Linux and Windows. This is **software** Vulkan; Windows did not have an
active Khronos validation layer (`validation_enabled: false`), and no physical
Windows GPU/driver performance claim follows. The later [combined P1/P3 Windows
graphics CI](https://github.com/emil28092005/Faset_Engine/actions/runs/35944875002)
at clean `4a3453e` passed 74/74 CTests, including real diagnostic navigation,
real Release export integration and all three relocated games for 120 frames.
The [parallel native/manual CI](https://github.com/emil28092005/Faset_Engine/actions/runs/35944874993)
passed on Linux and Windows. The physical-GPU Linux reference above had active
validation; Windows graphics used pinned SwiftShader.

## P1 acceptance record

| Area | Evidence and current limit |
| --- | --- |
| Lua | Linux native and Release [relocated Lua result](../lua-release-2026-09-24/README.md), repeated in the [clean combined three-game Release reference](final-release/README.md); Windows CPU/SwiftShader [combined 74-test suite](https://github.com/emil28092005/Faset_Engine/actions/runs/35944875002) and [relocated Lua-only Release report](windows-4ac02ee/playable-report.json), 120 completed frames. |
| Cache and rollback | [Five verified hits](report.json), `build_cache` and `build_schema_publication` contracts for headers, toolchain, schema/shader corruption and source races; the raw failed/recovered build retains the previous pointer. |
| Diagnostics | `build_diagnostics` fixtures cover Clang, clang-cl, Lua, Unicode and unparseable output. The raw real failure above found a staged-source mapping gap; [the replay](diagnostic-replay.json), parser case, warning-flood case and real BuildService integration assertion cover the correction. |
| Starters/navigation | [Four runnable C++/Lua × 2D/3D starters](../p1-starters-linux-2026-09-24.md) plus launcher/CLI and source-open tests. |
| Autosave | `editor_session_settings`, UI tests and [workspace behavior](../../manual/editor/workspace.md#create-and-save-a-scene): idle coalescing, unnamed Save As, external/revision conflicts, Undo and recovery. |
| Iteration | This Debug report, 179 raw files, 3,000-frame and seeded larger-scene profiles, plus the [clean combined Release reference](final-release/README.md) with three relocated games and exact 2D/3D budgets. The allocation budget miss is recorded; no CI timing gates. |
| Documentation | The English Manual describes build/reload, templates, navigation, autosave and profile interpretation; strict MkDocs and full platform test totals are recorded in the implementation journal. |

The [profiling guide](../../manual/editor/profiling.md#measure-editor-c-and-lua-iteration)
documents how to repeat the run and interpret its deliberately distinct timing
labels. C++ hot reload, state-preserving Lua reload and Player-world MCP are not
part of this P1 contract.
