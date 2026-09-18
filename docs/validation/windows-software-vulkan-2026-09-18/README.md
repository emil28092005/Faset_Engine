# Windows software Vulkan acceptance

[The final source record](final-source/evidence.json) documents a successful fresh-checkout build of `4cb82556de31268d2bde73948dd1ff1b6c02f162` in [GitHub Actions run 35301244334](https://github.com/emil28092005/Faset_Engine/actions/runs/35301244334). The hosted Windows Server 2025 image supplied Visual Studio tools and Clang 20.1.8 (`clang-cl`). The job prepared the pinned Vulkan loader/SwiftShader toolchain, fetched the verified Slang compiler, configured the Editor with `FASET_DEBUG_IMGUI=ON`, then built the Editor, Player and tests. All **35 CTests passed**, with no failures or skips.

The [test summary](final-source/tests.json) preserves the concrete acceptance cases and selected output:

- `editor_ui_launcher`: Create/Open, Unicode project paths, validation, recent projects, directory browsing and keyboard navigation through the instrumented retained UI. This is developer setup and first-project evidence, not a retail-installer test.
- `render_window_lifecycle`: native SDL window resize, Unicode clipboard roundtrip, text-input rectangle conversion, 12 fresh frames while minimized, and restore. Real OS IME composition was not exercised.
- `editor_gui_mcp`: a visible native Editor plus MCP stdio shared authoring, conflict/Undo handling and 12 fresh PNG captures without loss of responsiveness.
- `editor_debug_overlay`: the optional ImGui font/triangle rendering, F12 toggling and pointer-event isolation, including gestures crossing the overlay boundary.

Both the BuildService export/incremental-build integration and [the checked-in playable project verifier](final-source/playable-report.json) passed. Each game was built in **Release** from its own C++ scripts, packaged, checked against its manifest, moved outside the engine tree into a `Faset Café 世界` path, and run from an unrelated working directory with its disposable source project unavailable. Each standalone Player passed CPU scene validation and rendered 120 frames at 1280×720. The 3D case includes a real imported Blender arch bundle. Exact source input hashes, generations, packaged file hashes, process results and captures are retained.

The [toolchain](final-source/toolchain.json) and [probe](final-source/probe.json) identify **SwiftShader Vulkan 1.3, a CPU device**. The Khronos validation layer was absent (`validation_enabled=false`); zero reported validation errors must not be read as validation-layer coverage. These records establish functional behavior on a hosted Windows VM, not physical desktop GPU performance, a manually operated desktop, or a large-workload benchmark. Timing fields retained in the original verifier report are diagnostic data only.

Original detailed job logs and raw frame profiles are available in the linked CI artifact while its retention period lasts. This folder keeps the compact acceptance evidence and hashes of the original CTest/CI logs.

Git stores these JSON records with LF line endings. Each retained-file `sha256` and
`size` describes those repository bytes; where the original Windows artifact used
CRLF, `original_artifact_sha256` and `original_artifact_size` preserve that separate
byte identity. The documentation correction after the first MVP tag records this
normalization explicitly; it does not change the test outcomes or packaged games.

The earlier [checkpoint 5 record](checkpoint5/evidence.json) preserves the independently successful run of `0f34b036313c011861dbfd5828ed45c4f7940b05`. The final source run additionally covers the relocated importer metadata fix through `assets_pipeline`, followed by the complete export matrix again. [2D capture](final-source/collect-2d.png) · [3D capture](final-source/collect-3d.png).
