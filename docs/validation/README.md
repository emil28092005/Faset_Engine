# Validation evidence

These files preserve bounded checks and their inputs. Each record states its source revision or working-tree limitation; a passing record does not certify later commits or every supported platform.

- [Linux offline build](offline-linux-2026-09-18.json): a fresh build from committed `d834cfad67cd81d8c4998b90c16791361ca8c0f8`, prefetched dependency inputs, and a network namespace without external connectivity. CPU tests ran; graphics execution is separate.
- [Linux Release export and rendering](linux-release-2026-09-18/README.md): two relocated playable games, Unicode paths, imported Blender geometry, raw frame profiles and exact package hashes. This measurement used an evolving working tree around `5ac4db4`, not a clean checkout of the current commit.
- [Live Blender reimport](blender-live-linux-2026-09-18.json): actual Blender 4.5.3 exports and two live Editor instances, preserving authoring/gameplay/physics while refreshing geometry.
- [Editor recovery](editor-recovery-linux-2026-09-18.json): killed Editor, explicit journal recovery, failed save/import/build and failed Player startup with last-good preservation.
- [Native window boundaries](native-window-linux-2026-09-18.json): XWayland lifecycle, isolated Unicode clipboard, SDL text-input rectangles and the explicit native Wayland restore limitation.
- [Linux native window and input boundaries](native-window-linux-2026-09-18.json): XWayland resize/minimize/restore and SDL text-input-area checks; isolated Xvfb Unicode clipboard roundtrip with restoration. Native Wayland restore remains unconfirmed; real OS IME composition was not tested.

The Linux records do not establish Windows correctness, physical Windows GPU performance, native Wayland restore support, or editor responsiveness under large workloads. Windows CI artifacts record their own commit and software Vulkan results. A skipped compositor operation is not a completed acceptance check.

Performance budgets below are proposals for P1 regression tracking, not completed release requirements. Repeat measurements on a controlled reference host and add representative content before treating them as engine-wide targets.
