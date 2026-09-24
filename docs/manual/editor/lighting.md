# Light a 3D scene

Select an entity in the **Scene** tree, choose **+ Add Component** in the
**Inspector**, and add **Light**. Its Transform places a point or spot light. A
spot light points along the entity's local negative Z axis; a directional light
uses the entity's orientation. Lights affect 3D meshes in linear PBR shading
before tone mapping. Sprites and Editor UI remain unlit.

| Light kind | Coverage | Shadow cost |
| --- | --- | --- |
| `directional` | A sun-like direction, independent of position | Up to four cascade tiles |
| `point` | All directions within `range` | Six local-atlas tiles, assigned together |
| `spot` | A cone within `range` | One local-atlas tile |

The Light component's fields are:

| Field | Default | Meaning |
| --- | --- | --- |
| `kind` | `directional` | `directional`, `point`, or `spot` |
| `enabled` | `true` | A disabled light contributes no illumination |
| `color` | `[1, 1, 1, 1]` | RGB illumination color; alpha is part of the schema color value |
| `intensity` | `1` | Nonnegative brightness |
| `range` | `10` | Positive reach of point and spot lights |
| `inner_angle` | `0.35` | Full-strength spot cone half-angle, in radians |
| `outer_angle` | `0.7` | Outer spot cone half-angle, in radians; must be at least `inner_angle` |
| `casts_shadow` | `true` | Allow this light to use its shadow atlas |
| `shadow_priority` | `0` | Higher local-light selection and shadow priority; does not change brightness |

The Inspector validates the spot angles together. Their allowed outer limit is
below π/2 radians. A point light does not depend on the entity's rotation.
After editing the light or Transform, save the scene as usual. [Editor
workspace](workspace.md) explains Inspector editing, Undo, and save conflicts.

## Sun shadows and compatibility

With a 3D scene camera, a shadow-casting directional light uses four cascades
covering the camera near plane through at most **80 world units**, or the camera
far plane if it is closer. Faset blends samples near cascade splits and snaps
each shadow projection to texels to reduce shimmer during small camera moves.
Objects outside the camera view can still cast into a visible receiver: shadow
visibility uses each light's view and the source mesh's LOD 0, separately from
the main camera's Direct or GPU visibility result. A low-level renderer Snapshot
without an explicit camera frustum uses one compatibility sun view.

Only one enabled directional light is used. If there are several, Faset chooses
the one with the smallest stable entity ID and reports the ignored lights. A
scene with **no Light component** retains the older white sun. Adding any Light
component, including a disabled one, suppresses that compatibility sun. Thus a
local-only scene does not receive an unexpected directional light.

## Local shadow capacity and fallbacks

The renderer accepts at most **128** local lights per frame. It sorts candidates
by `shadow_priority` (highest first), then projected influence, then stable ID.
The `omitted_local_lights` counter reports lights beyond this limit; an omitted
light contributes no illumination. All authored light records are validated,
including candidates past the limit.

The separate local shadow atlas has **16 tiles**. A spot consumes one; a point
consumes six or none. Sun cascades and local shadows share a maximum of **4096
caster draws** per frame. A light whose shadow group does not fit the remaining
tiles or draw budget still illuminates, **without a shadow**. Disabling
`casts_shadow` also keeps illumination while skipping that light's shadow work.
The renderer reports requested faces, rendered faces, tiles, and drops by cause
in [Diagnostics](diagnostics.md) and the [Player profile](profiling.md).

Faset requires sampled D32 depth images for the renderer itself. On a supported
device, it uses separate sun and local atlases, normally 2048×2048 pixels each.
If an optional atlas cannot use that size, the renderer tries 1024×1024. When
the local atlas cannot be allocated, or the sun atlas cannot be allocated but
a 1×1 sampled D32 fallback image can, affected lights remain unshadowed and
report unavailable shadow views. A device without sampled D32 support cannot
start this renderer. Scheduled atlas tiles are cleared and redrawn each frame;
there is no persistent shadow cache yet.
Sprite-only scenes, a missing sun, and a sun with `casts_shadow: false` skip sun
shadow raster work.

## Local-light rendering path

The normal `Auto` setting uses the measured forward light scan. It is the
current default for Editor and Player. A C++ renderer integration can explicitly
set `RendererConfig::lighting_mode = LightingMode::Tiled` to build depth-free
16×16 screen-tile lists on a capable Vulkan device. Each tile stores at most
64 light indices in stable order. If more lights touch a tile, its fragment
shader scans the complete submitted list, so an overflow never removes
illumination. The path falls back to forward when no local lights are present
or the compute/buffer requirements are unavailable. Sprites and UI stay unlit.

This explicit path can help when light ranges occupy small parts of the screen;
it costs extra work when nearly every light covers nearly every tile. The
fixed dense benchmark was slower after including tile construction, so there
is no automatic scene-dependent switch yet. The Player profile reports
`effective_lighting_path`, tile GPU time and grid size; optional Editor
diagnostics also report stored candidates and overflowing tiles. See
[Profiling](profiling.md) and the [measured Forward+ study](https://github.com/emil28092005/Faset_Engine/blob/main/docs/studies/23-p3-forward-plus-2026-09-24.md).

## Add a point light through MCP

MCP edits the **Editor document**, not entities in a running game. Use
`faset_schema` to inspect the current field IDs, then send a `faset_scene_edit`
batch with the document ID, its current revision, and a target entity ID:

```json
{
  "document": "REPLACE_WITH_DOCUMENT_ID",
  "revision": 4,
  "idempotency_key": "add-red-point-light",
  "operations": [{
    "op": "component.add",
    "entity": "REPLACE_WITH_ENTITY_ID",
    "type": "faset.light",
    "fields": {
      "kind": "point",
      "color": [1, 0.15, 0.1, 1],
      "intensity": 8,
      "range": 6,
      "shadow_priority": 2
    }
  }]
}
```

Move the entity with its Transform component. `component.add` fills omitted
fields from the schema; `component.set` changes an existing field. Save the
document with `faset_document_save`. [MCP and command line](mcp.md) covers
revisions, retries, and transactions.

## Supply lights directly from C++

Code that constructs a renderer `faset::render::Snapshot` can supply lights
directly. Set `authored_lights_present` even when the only authored Light is
disabled, so the renderer does not synthesize the compatibility sun. Use stable,
unique IDs for deterministic capacity decisions:

```cpp
faset::render::Snapshot snapshot;
snapshot.authored_lights_present = true;

faset::render::LocalLight point;
point.kind = faset::render::LocalLight::Kind::Point;
point.stable_id = "level/torch";
point.position = {-2, 1.5f, 0};
point.color = {1, 0.3f, 0.1f, 1};
point.intensity = 8;
point.range = 6;
point.shadow_priority = 2;
snapshot.local_lights.push_back(point);

faset::render::LocalLight spot;
spot.kind = faset::render::LocalLight::Kind::Spot;
spot.stable_id = "level/lamp";
spot.position = {2, 3, 0};
spot.direction = {0, -1, 0};
spot.inner_angle = 0.25f;
spot.outer_angle = 0.55f;
spot.intensity = 5;
spot.range = 9;
snapshot.local_lights.push_back(spot);
```

This is the **renderer Snapshot API**, not a gameplay `Update()` method. The
current gameplay scripting API does not expose live Light-component creation or
modification; author lights in the Inspector or through Editor MCP. See
[Gameplay scripting](../scripting/index.md) for the APIs available to game code.
