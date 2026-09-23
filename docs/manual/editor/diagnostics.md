# Developer diagnostics

## Build errors and source navigation

Build and export jobs report compiler, clang-cl/MSVC, and Lua errors as structured
Console rows. Each row shows severity, message, and a one-based source location.
Choose **Open source** to open a file under the current project's `Scripts/`
directory at that line and column. Diagnostics from engine or external files remain
visible but have no source action. The **Jobs** tab shows whether a verified build
generation was reused, the elapsed phases, and an expandable raw process log;
**Copy full log** retains the complete available output for troubleshooting.

The same action is available over Editor MCP:

```json
{"command":"faset_source_open","arguments":{"path":"Scripts/Gameplay.cpp","line":17,"column":4}}
```

`faset_script_open` remains Lua-only. Both commands use the project's optional
`editor.script_editor` argv array, or Zed by default. The tokens `{file}`,
`{line}`, `{column}`, and `{project}` may appear within one argument, for
example `["zed", "{file}:{line}:{column}"]`. Faset passes each argument
literally to the editor process without shell expansion. An invalid editor
command produces a source-editor error without changing the build result.

The optional Dear ImGui overlay shows renderer counters inside the Editor. The Editor's normal interface remains the retained Faset UI. Enable the diagnostic build explicitly:

```sh
cmake --preset linux-debug -DFASET_DEBUG_IMGUI=ON
cmake --build --preset linux-debug --parallel 4
build/linux-debug/faset_editor --project examples/projects/collect-3d --gui
```

On Windows, use `windows-debug` for both presets and `build/windows-debug/faset_editor.exe`. Press **F12** to show or hide the panel. Drag its title bar to move it; **Freeze counters** holds a completed-frame sample for inspection. Closing the panel does not stop rendering. Pointer gestures inside the overlay are kept out of the authoring UI.

Use the **Visibility** selector to compare **Direct**, **GPU frustum**, and **GPU occlusion** on the same open scene. This is a live renderer setting for the Editor viewport; it does not change the scene or exported game. The selected mode is independent of **Freeze counters**. The counters describe the previous completed frame, so render one more frame after changing modes before reading them. **Effective path** names the algorithm that actually ran. A **Fallback from** line appears when device or target capabilities prevent the selected mode; for example, GPU occlusion may use GPU frustum if HZB is unavailable.

The panel reports the previous completed frame: renderer wall time, GPU timestamp time where available, synchronous readback time, draw calls, packed vertices, culled meshes, textures, explicit Vulkan allocation sizes, actual validation availability/errors, and GPU pass-label count. It also shows whether GPU visibility ran, submitted indirect bins, visible instances, frustum rejects, deferred and post-pass visible instances, HZB history validity, counts per prepared LOD level, and GPU pass timings where available. GPU counts are explicitly marked unavailable until the first frame rendered with diagnostics open; only a displayed zero is a measured zero. **Previous HZB history: invalid** is expected after a camera cut or resize until compatible depth history is available. A current HZB preview can still exist after that first frame because it was built from the current depth. Renderer wall time includes waiting for GPU work; it is not thread CPU usage. Memory excludes driver-internal allocations. The overlay itself adds drawing work, so hide it for a baseline performance measurement.

In **GPU occlusion** mode, enable **Show HZB** to inspect the current grayscale depth pyramid. The **Mip** slider selects a pyramid level; the preview starts at mip 3 to keep its readback small. A larger mip number shows coarser depth. The preview reads the HZB only while the panel and toggle are open, and only once per completed frame or mip change. Switching it off or closing the panel releases the preview; its GPU texture retires when the next frame begins. Opening diagnostics also enables readback of GPU visibility counters, which is disabled again when the panel closes. Disable the HZB preview for performance comparisons: its diagnostic copy and texture upload add GPU and CPU work. **Freeze counters** does not freeze the HZB image.

The Vulkan backend emits `VK_EXT_debug_utils` labels for `ShadowMap`, `ForwardAndUI`, `Readback`, and, when presenting, `Presentation`. A graphics capture tool that supports this extension can identify those command-buffer regions. Labels remain available without the Khronos validation layer when the extension is exposed; unsupported systems continue rendering and report labels unavailable. A submitted-label count confirms calls were emitted, not that an external capture tool was tested.

This module is disabled by default and is linked only to the graphical Editor and its dedicated test when enabled. Player and exported games do not link ImGui. No overlay control changes authoring documents, gameplay state or export settings.

Run `ctest --test-dir build/linux-debug -R '^editor_debug_overlay$' --output-on-failure` in an enabled build to check actual ImGui geometry/font rendering, F12 toggling, pointer isolation and restoration of the underlying frame. The regular renderer pixel test also verifies GPU labels and clipped UI triangles.
