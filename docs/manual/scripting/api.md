# Runtime API reference

Include `<faset/runtime/Runtime.hpp>` and use namespace `faset::runtime`. This is the implemented C++ surface used by the compiled tutorials. The runtime is sequential; call it from its owning thread.

## Register behavior

`void Runtime::registerBehavior(std::string componentType, Behavior behavior)` registers callbacks before scene loading. An empty or duplicate type and registration while entities are present or a callback is running are rejected. An empty or cleared world can register additional types.

`Behavior::Callback` is `std::function<void(Runtime&, EntityHandle, double)>`. Assign it to any of `onStart`, `fixedUpdate`, `update`, `lateUpdate`, and `onDestroy`. Unassigned members do nothing. `Behavior::onCollision` instead accepts `(Runtime&, EntityHandle, const CollisionEvent&)`.

See [callback order](lifecycle.md) and the complete [registration example](first-behavior.md).

## Resolve identity

- `EntityHandle find(const std::string& persistentId) const` returns a handle, or an empty handle when absent.
- `bool valid(EntityHandle) const noexcept` checks session, entity validity, and generation.
- `std::uint64_t session() const noexcept` identifies this runtime session, not the saved scene.

A handle contains `session`, `slot`, and `generation`. Its Boolean conversion says it is nonempty; it **does not** prove that the object is still alive. Call `valid` before using a retained handle. A handle from another runtime or an earlier `load` is rejected.

## Read configuration and poses

- `nlohmann::json fields(EntityHandle, const std::string& componentType) const` returns a configuration copy. It throws if the type is absent.
- `Transform transform(EntityHandle) const` returns a simulation-pose copy.
- `Transform presentation(EntityHandle) const` returns the pose prepared for display.
- `void setTransform(EntityHandle, const Transform&)` updates a non-physical object.
- `void setPresentation(EntityHandle, const Transform&)` updates presentation only, during `lateUpdate`.

`Transform` has `position`, `rotation`, and `scale`, each a `std::array<float, 3>`. Position is in metres; rotation is XYZ Euler radians with matrix composition `Rz * Ry * Rx`; scale is a multiplier. The pose is local to its scene parent. The renderer composes the hierarchy. The initial physics adapters require root objects.

These are values, not borrowed component pointers. Changing a returned copy has no effect until an appropriate setter is called. Presentation writes do not alter physics or the saved scene.

## Control a rigid body

- `Vec3 velocity(EntityHandle) const` reads metres per second. A 2D body returns Z = 0.
- `void setVelocity(EntityHandle, Vec3)` sets linear velocity; it does not take a displacement.
- `void applyImpulse(EntityHandle, Vec3)` applies an impulse at the centre and wakes the body.
- `void teleport(EntityHandle, const Transform&)` changes the pose discontinuously, wakes the body, and resets interpolation/contact-query history. It does not zero velocity.
- `bool grounded(EntityHandle) const` tests support using recent native contact normals.

All methods require a valid handle. Velocity, impulse, and grounded queries also require a physics body; `teleport` accepts physical and non-physical objects. Ordinary `setTransform` is rejected for physical objects, including static and kinematic bodies. See [physics](physics.md) for dimensions, tolerances, and collider limits.

## Input and collision events

`InputState input() const noexcept` returns `horizontal`, `vertical`, `jumpPressed`, and `interactPressed`. The Player maps A/D and left/right arrows to horizontal input, W/S and up/down arrows to vertical input, Space to jump, and E to interact. The gameplay module decides what these actions do.

`CollisionEvent` contains `first`, `second`, and `began`. `onCollision` receives an event during post-physics delivery. Its reference lasts for that callback; copy the event if you need to retain it, then recheck retained handles before later use.

`const std::vector<CollisionEvent>& collisions() const noexcept` exposes the most recently completed tick's events. The vector is replaced on a later tick or scene replacement. Polling only once per rendered frame can miss an earlier tick in a multi-tick frame; use callbacks when every delivered event matters. This is contact begin/end notification, not a general event bus or a contact-normal query.

## Queue structural changes

- `void spawn(nlohmann::json entity)` queues an entity record with `id`, `name`, `parent`, and `components`.
- `void destroy(EntityHandle)` queues removal of the object and its descendants.
- `void addComponent(EntityHandle, nlohmann::json component)` queues a full component record.
- `void removeComponent(EntityHandle, const std::string& componentType)` queues removal by type.

Changes are applied in FIFO order at the next fixed-tick barrier. `spawn` does not return an immediately usable handle; use `find(id)` after application. A spawned child's parent must already exist when its command is applied. These commands are individual runtime operations, not an atomic authoring batch with Undo.

Immediate validation errors throw. Deferred failures are recorded in diagnostics; a stale command does not revive an object. IDs and component types must not collide. The [spawning example](examples.md#spawn-and-destroy-on-safe-boundaries) demonstrates the timing.

## Drive a world or a test

`Runtime(RuntimeConfig = {})` constructs the controller. `load(const nlohmann::json&)` validates and prepares a scene, creates its entities, then calls initial callbacks. Invalid scene data leaves the preceding world intact. `clear()` destroys the current entities and invalidates their session handles.

Direct callers can include `<faset/runtime/schema.hpp>` and call
`validate_scene_schemas(scene, gameplaySchema)` before `load`. The Player does this
automatically: each custom TypeId/version must match its linked schema. Generic
`Runtime` accepts opaque custom data with positive versions, including data-only
components; built-ins use version 1. This helper verifies identity/version, without
automatic migrations or authoring-style custom-field constraint validation.

`FrameStats advance(double elapsedSeconds, InputState = {})` advances fixed ticks, frame callbacks, interpolation, and late callbacks. `singleStep(InputState = {})` advances one fixed tick. `setPaused(bool)` clears accumulated time and resets presentation history; `paused()` reports this local state. Do not call load/clear/advance recursively from a callback.

`RuntimeConfig` defaults to `fixedDelta = 1.0 / 60.0`, `maxCatchUpTicks = 4`, `physicsSubsteps = 4`, and `gravity = {0, -9.81f, 0}`. `FrameStats` reports fixed ticks performed, dropped time, interpolation fraction, and total tick count.

`snapshot()` returns a value snapshot for rendering; `snapshotJson()` provides its JSON representation. `diagnostics()` returns a read-only vector of runtime messages. These are native C++ APIs for the Player and tests, **not MCP endpoints**.

## Editor data migrations

Gameplay schema versions describe saved component data. When a field changes units
or meaning, increase the type's `version` and include declarative `migrations` in
that type returned by `gameplay::schema()`. For example, this type declaration
converts version 1 speed values from centimetres per second to metres per second:

```json
{
  "id": "game.mover",
  "version": 2,
  "fields": {
    "speed": {"type": "number", "default": 2.5, "min": 0, "max": 10},
    "enabled": {"type": "boolean", "default": true}
  },
  "migrations": [
    {
      "from_version": 1,
      "fields": {
        "speed": {"scale": 0.01},
        "enabled": {"default": true}
      }
    }
  ]
}
```

Build C++ to export and validate the declaration. The Editor reads these rules
from the same schema manifest as field metadata; it does not load the gameplay
library or execute a migration callback. Invalid metadata or migration rules fail
the build before replacing the last published binary/schema generation.

Each step upgrades `from_version` to the next integer version. A component at
version 1 needs both steps 1 and 2 to reach version 3. Empty `fields` explicitly
allows a version step with no value conversion. Supported field operations are:

- `default`: insert a value only when the field is absent.
- `scale`: multiply an existing numeric field by a finite number.
- `require_manual`: when `true`, stop if the field is present, so incompatible data
  requires an explicit manual conversion.

Rules preserve component/entity IDs and fields they do not mention. The final
values must satisfy the current field schema. Unsupported operations, repeated
steps, invalid version ranges, invalid rules and non-finite scale values are
rejected when the schema is loaded. Arithmetic overflow during conversion also
fails without applying the transaction.

Opening or recovering a scene preserves older component versions as opaque data;
it never migrates them automatically. Missing rules therefore do not prevent
opening the scene. Choose **Migrate to v…** in the Inspector after rebuilding the
schema, or use the same editor's `faset_scene_edit` operation with the current
document revision:

```json
{
  "document": "document-id",
  "revision": 3,
  "operations": [
    {"op": "component.migrate", "entity": "entity-id", "component": "component-id"}
  ]
}
```

The whole batch is one Undo step and is written to the recovery journal. Save
explicitly to update the scene file. Missing steps, a manual-conversion requirement
or a validation error leave the document and revision unchanged. Future versions
cannot be downgraded. The same operation accepts an `instance` ID when `entity`
and `component` identify a top-level instance-local addition using its original
stored IDs, rather than resolved preview IDs.

Inherited components belong to their source document: open that source to migrate
them. Sparse field overrides in other instances are not automatically converted;
review and explicitly update overrides when changing a field's units or meaning.
The Player performs no migration and still requires the scene version to match its
linked gameplay schema before Play or exported-game validation.

## Inspect a running Player locally

The Player has local development controls in addition to gameplay input: **P** toggles
pause, **N** steps one fixed tick while paused, and **Escape** closes the Player.
**F3** toggles physics-box outlines; `--debug-physics` enables them from startup.
Static bodies are green, kinematic bodies orange, and dynamic bodies cyan.

These outlines use current physics poses and collider half-extents multiplied by the
absolute transform scale. They can differ slightly from an interpolated visible mesh.
The initial adapters support root-only boxes: four outline edges in 2D, twelve in 3D.
This view does not show contact normals, broad-phase cells, or arbitrary mesh colliders.

To record measurements, run the Player with `--profile measurements.json --frames 240`.
A profile requires an explicit count from 1 to 100,000. As with all bounded Player
runs, simulation uses the configured fixed delta each frame; `--headless` renders
through offscreen Vulkan. The report contains measured durations rather than treating
that simulation delta as frame time. See [Player profiling](../editor/profiling.md)
for startup, CPU/GPU timing, percentiles, and their limits. These flags do not add MCP
to the Player.
