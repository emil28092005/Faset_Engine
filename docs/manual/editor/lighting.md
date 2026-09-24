# Add lights to a 3D scene

Add a **Light** component to a scene entity. The entity's transform places a point
or spot light; its rotation aims a spot light along local negative Z. A directional
light uses the entity's orientation. Light colors and intensity contribute to the
mesh's linear PBR illumination before tone mapping. Sprites and UI retain their
unlit tint.

The version-1 `faset.light` component has three `kind` values:

| Kind | Position and direction | Useful fields |
| --- | --- | --- |
| `directional` | Direction from the entity transform | `color`, `intensity`, `casts_shadow` |
| `point` | Position from the entity transform; illuminates every direction | `color`, `intensity`, `range` |
| `spot` | Position and local negative-Z direction | `color`, `intensity`, `range`, `inner_angle`, `outer_angle` |

Angles are radians. A spot's inner angle must not exceed its outer angle. Intensity
must be nonnegative and range positive. `enabled: false` keeps the component in the
scene without contributing light. The `shadow_priority` integer is reserved for the
bounded local-shadow scheduler; it does not change brightness.

In the current rendering checkpoint, one enabled directional light can cast the
existing single-map shadow. Point and spot lights illuminate meshes but do not yet
cast shadows. The [P3 lighting plan](https://github.com/emil28092005/Faset_Engine/blob/main/docs/superpowers/plans/2026-09-24-p3-lighting.md)
tracks cascades and the bounded local-shadow atlas. A scene with no Light component
keeps the legacy white sun so older projects retain their appearance. Adding any
Light component, even a disabled one, turns off that compatibility fallback. If
several directionals are enabled, Faset chooses the one with the smallest stable
entity ID and reports a diagnostic for the others.

## Author a point light through MCP

Use `faset_schema` to inspect the current field IDs, then send a `faset_scene_edit`
batch with the document ID, current revision, and target entity ID. For example:

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
      "range": 6
    }
  }]
}
```

Move the entity with its Transform component. `component.add` fills any omitted
light fields from the version-1 schema; use `component.set` for later edits. See
[MCP and command line](mcp.md) for revision and retry handling.

## Supply lights directly from C++

When building a `faset::render::Snapshot` yourself, set
`authored_lights_present` to suppress the compatibility sun in a local-only scene.
Provide a stable ID for each light so future shadow scheduling remains independent
of submission order.

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

The renderer submits at most 128 local lights per frame in stable-ID order. Later
P3 work adds explicit overflow diagnostics and measured light-list optimization.
