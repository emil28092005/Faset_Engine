# Final Windows SwiftShader evidence

The [Windows graphics CI run](https://github.com/emil28092005/Faset_Engine/actions/runs/35949830251)
completed successfully on clean source revision
`ed523c61c106b8e19614e4d56f0b5d39b8392478`. Its CTest step passed
**78/78 tests**. The compressed [CTest log](LastTest.log.gz) retains individual
test output. The workflow also passed the Release 2D/3D export integration;
its [2D](integration-result-2.json) and [3D](integration-result-3.json) result
records are retained here.

The [playable export report](playable-report.json) records `engine_dirty=false`,
the Editor SHA-256, and three successful Release exports: C++ `collect-2d`,
C++ `collect-3d` (including the Blender asset import), and Lua-only `lua`.
Each package was relocated to a Unicode-named directory, ran **120/120**
offscreen frames on SwiftShader, and reported that its source project path was
unavailable. The [Vulkan probe](vulkan-probe.json) identifies the software
device; [toolchain details](toolchain.json), package manifests, per-frame
profiles, validation and run records are retained alongside this page.

The Windows packages report `validation_enabled=false`: the pinned SwiftShader
CI verifies functional rendering but does not substitute for a physical Windows
GPU run with an active Vulkan validation layer. Linux physical-GPU validation
is recorded in the [parent dossier](../README.md).

The data were copied unchanged from the workflow's `windows-graphics-evidence`
artifact. Verify the retained files with `sha256sum -c SHA256SUMS.txt` from this
directory. The `.gz` CTest log was compressed without a timestamp.
