# Runtime API reference

Include `<faset/runtime/Runtime.hpp>` and use namespace `faset::runtime`. This is the implemented C++ surface used by the compiled tutorials. The runtime is sequential; call it from its owning thread.

## Register behavior

`void Runtime::registerBehavior(std::string componentType, Behavior behavior)` registers callbacks before scene loading. An empty or duplicate type, registration during a callback, and registration after entities have loaded are rejected.

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

These methods require a valid handle and a physics body. Ordinary `setTransform` is rejected for physical objects, including static and kinematic bodies. See [physics](physics.md) for dimensions, tolerances, and collider limits.

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

`FrameStats advance(double elapsedSeconds, InputState = {})` advances fixed ticks, frame callbacks, interpolation, and late callbacks. `singleStep(InputState = {})` advances one fixed tick. `setPaused(bool)` clears accumulated time and resets presentation history; `paused()` reports this local state. Do not call load/clear/advance recursively from a callback.

`RuntimeConfig` defaults to `fixedDelta = 1.0 / 60.0`, `maxCatchUpTicks = 4`, `physicsSubsteps = 4`, and `gravity = {0, -9.81f, 0}`. `FrameStats` reports fixed ticks performed, dropped time, interpolation fraction, and total tick count.

`snapshot()` returns a value snapshot for rendering; `snapshotJson()` provides its JSON representation. `diagnostics()` returns a read-only vector of runtime messages. These are native C++ APIs for the Player and tests, **not MCP endpoints**.
