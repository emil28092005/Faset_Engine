# GPU visibility and prepared mesh LOD

Faset can draw opaque static triangle meshes through GPU frustum culling and,
optionally, two-pass HZB occlusion. The original direct renderer remains available
for comparison. In the Editor, build with `FASET_DEBUG_IMGUI=ON`, press **F12**,
and choose **Direct**, **GPU frustum**, or **GPU occlusion** in Diagnostics. **Path:
active** confirms that the selected GPU path actually ran. See
[developer diagnostics](diagnostics.md) for the HZB preview and counters.

The current GPU path groups instances with the same mesh and texture into fixed
indirect draw bins. A previous-frame HZB can defer an object, but the current-frame
post pass checks it again before the final image. Sprites, editor UI, transparent
meshes, and shadow casters keep their existing ordered or shadow paths. This option
does not require ray tracing or mesh shaders.

## Supply prepared LODs from C++

`DrawItem::mesh` is LOD 0. Set `lod_meshes[0]` to a prepared LOD 1 mesh,
`lod_meshes[1]` to LOD 2, and so on. These are actual `Mesh` objects with a
complete triangle list; Faset does not simplify a source mesh automatically.
Keep a stable `instance_key` across frames. Give different scene objects different
keys, and retain the same shared mesh objects instead of rebuilding them every
frame. For a manually constructed `Snapshot`, supply both `projection` and
`view_projection`; set `view_id` for each camera and `camera_cut` when switching
shots. The Player's scene extraction fills the camera fields and instance keys.

```cpp
faset::render::DrawItem draw;
draw.mesh = prepared_high_detail;
draw.lod_meshes = {prepared_medium_detail, prepared_low_detail};
draw.instance_key = "scene/object-id/primitive-id";
draw.model = faset::render::transform({0, 0, 0});
snapshot.draws.push_back(std::move(draw));
```

LOD selection uses projected screen size. The first transition is 192 pixels;
each additional level halves that threshold. A 12% hysteresis band avoids
switching back and forth near a boundary. Missing levels fall back to an available
mesh. Changing level invalidates that instance's temporal occlusion state while
preserving its logical key. `FrameStats::lod_counts` reports selected levels 0–3
(higher levels are included in the final bucket).

For a runnable C++ example, build `faset_p2_visibility_example` and capture the
same scene in any mode:

```sh
cmake --build --preset linux-debug --target faset_p2_visibility_example
build/linux-debug/faset_p2_visibility_example occlusion /tmp/p2-visibility.ppm
build/linux-debug/faset_p2_visibility_example direct /tmp/p2-direct.ppm
```

The example creates a fine cube and a prepared coarse tetrahedron in
[`examples/renderer/p2_visibility.cpp`](https://github.com/emil28092005/Faset_Engine/blob/main/examples/renderer/p2_visibility.cpp).
Imported GLB geometry can be supplied as prepared levels by C++ integration,
but automatic LOD generation and assigning a GLB's alternate meshes as LODs in
the Inspector are not implemented yet.

## Measure before choosing a mode

The GPU route saves per-instance CPU vertex transformation and direct draw
submission, but its compute passes and indirect rendering have a cost. Keep the
same scene, resolution and camera path when comparing modes. Hide Diagnostics for
an ordinary Player measurement: opening it enables GPU counter readback, while
**Show HZB** adds a separate image copy. The renderer currently captures and
waits for every frame even without this overlay. Read
[profiling and measurements](profiling.md) and the
[P2 acceptance study](https://github.com/emil28092005/Faset_Engine/blob/main/docs/studies/19-p2-gpu-visibility-acceptance.md)
for the measured scope and limitations.

## Select a mode in the Player

The exported Player uses Direct by default. Choose a GPU mode for one run with
`--visibility`; the same option works for a development Player and a relocated
standalone game:

```sh
./faset_player --visibility gpu-frustum
./faset_player --visibility gpu-occlusion --headless --frames 120 \
  --profile visibility-profile.json
```

Accepted values are `direct`, `gpu-frustum`, and `gpu-occlusion`. The profile
records the requested `visibility_mode` and each frame's
`gpu_visibility_active` status. If the device lacks the GPU path, the Player
reports a Direct fallback; selecting a mode alone does not prove that it ran.
The Editor's Diagnostics selector changes only its viewport and does not save a
Player setting. An exported game contains the P2 shader bundle and does not need
the Slang compiler at runtime.
