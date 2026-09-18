# Developer diagnostics

The optional Dear ImGui overlay shows renderer counters inside the Editor. The Editor's normal interface remains the retained Faset UI. Enable the diagnostic build explicitly:

```sh
cmake --preset linux-debug -DFASET_DEBUG_IMGUI=ON
cmake --build --preset linux-debug --parallel 4
build/linux-debug/faset_editor --project examples/projects/collect-3d --gui
```

On Windows, use `windows-debug` for both presets and `build/windows-debug/faset_editor.exe`. Press **F12** to show or hide the panel. Drag its title bar to move it; **Freeze counters** holds a completed-frame sample for inspection. Closing the panel does not stop rendering. Pointer gestures inside the overlay are kept out of the authoring UI.

The panel reports the previous completed frame: renderer wall time, GPU timestamp time where available, synchronous readback time, draw calls, packed vertices, culled meshes, textures, explicit Vulkan allocation sizes, actual validation availability/errors, and GPU pass-label count. Renderer wall time includes waiting for GPU work; it is not thread CPU usage. Memory excludes driver-internal allocations. The overlay itself adds drawing work, so hide it for a baseline performance measurement.

The Vulkan backend emits `VK_EXT_debug_utils` labels for `ShadowMap`, `ForwardAndUI`, `Readback`, and, when presenting, `Presentation`. A graphics capture tool that supports this extension can identify those command-buffer regions. Labels remain available without the Khronos validation layer when the extension is exposed; unsupported systems continue rendering and report labels unavailable. A submitted-label count confirms calls were emitted, not that an external capture tool was tested.

This module is disabled by default and is linked only to the graphical Editor and its dedicated test when enabled. Player and exported games do not link ImGui. No overlay control changes authoring documents, gameplay state or export settings.

Run `ctest --test-dir build/linux-debug -R '^editor_debug_overlay$' --output-on-failure` in an enabled build to check actual ImGui geometry/font rendering, F12 toggling, pointer isolation and restoration of the underlying frame. The regular renderer pixel test also verifies GPU labels and clipped UI triangles.
