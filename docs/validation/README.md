# Validation evidence

These files preserve bounded checks and their inputs. Each record states its source revision or working-tree limitation; a passing record does not certify later commits or every supported platform.

- [MVP acceptance dossier](mvp-acceptance.md): criterion-by-criterion closure, tested revisions and remaining compatibility coverage.
- [P2 GPU visibility Linux evidence](p2-gpu-visibility-2026-09-23/README.md): Debug/Release GPU acceptance, lavapipe functional checks, relocated Player exports, and explicit platform/performance limits.
- [Windows software Vulkan](windows-software-vulkan-2026-09-18/README.md): fresh native build, 35 tests, launcher/window/MCP workflows and both relocated Release games on SwiftShader.
- [Checkpoint 5 Linux acceptance](checkpoint5-linux-2026-09-18/README.md): clean offline source build, first Editor launch, exact-candidate standalone games and live Blender checks.
- [Final Linux source checks](final-linux-2026-09-18/README.md): `4cb8255` integrated test results and both Release games after the asset-relocation correction, including package manifests and standalone captures.
- [Linux offline build](offline-linux-2026-09-18.json): a fresh build from committed `d834cfad67cd81d8c4998b90c16791361ca8c0f8`, prefetched dependency inputs, and a network namespace without external connectivity. CPU tests ran; graphics execution is separate.
- [Linux Release export and rendering](linux-release-2026-09-18/README.md): two relocated playable games, Unicode paths, imported Blender geometry, raw frame profiles and exact package hashes. This measurement used an evolving working tree around `5ac4db4`, not a clean checkout of the current commit.
- [Live Blender reimport](blender-live-linux-2026-09-18.json): actual Blender 4.5.3 exports and two live Editor instances, preserving authoring/gameplay/physics while refreshing geometry.
- [Editor recovery](editor-recovery-linux-2026-09-18.json): killed Editor, explicit journal recovery, failed save/import/build and failed Player startup with last-good preservation.
- [Linux native window and input boundaries](native-window-linux-2026-09-18.json): XWayland resize/minimize/restore and SDL text-input-area checks; isolated Xvfb Unicode clipboard roundtrip with restoration. Native Wayland restore remains unconfirmed; real OS IME composition was not tested.

The Linux records do not establish Windows correctness, physical Windows GPU performance, native Wayland restore support, or editor responsiveness under large workloads. Windows CI artifacts record their own commit and software Vulkan results. A skipped compositor operation is not a completed acceptance check.

The thresholds in the [Release baseline](linux-release-2026-09-18/README.md) are the initial P1 tracking budgets for its exact scenes and reference host. They are not engine-wide guarantees or enforced MVP performance gates. Repeat measurements on a controlled reference host and add representative content before expanding their scope.
