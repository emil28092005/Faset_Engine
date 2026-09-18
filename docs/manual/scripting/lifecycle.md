# Frame and physics updates

The C++ member names are `onStart`, `fixedUpdate`, `update`, `lateUpdate`, and `onDestroy`. Design discussions may call the corresponding phases OnStart, FixedUpdate, Update, LateUpdate, and OnDestroy. Use the **camelCase member names** in code.

Each ordinary callback receives `(Runtime& game, EntityHandle self, double delta)`. Register only the callbacks you need. `onStart` and `onDestroy` receive a zero `delta`; update callbacks receive seconds. `onCollision` has a separate event signature described in the [API reference](api.md).

## Object lifetime

`onStart` runs once after an object and its components exist. All objects in the initial scene are created before their initial callbacks run. A spawn queued by `onStart` becomes visible at the next fixed-tick barrier, not during the callback that requested it.

`onDestroy` runs while that object's handle and allowed component data are still valid. Clean up subscriptions or external C++ state there. After removal, `valid(oldHandle)` returns false. Removing a behavior component also runs that component's `onDestroy`. Clearing or replacing a scene runs destruction callbacks; a new scene uses a new session identity.

Registering a callback does not make captured pointers safe. A lambda that stores a reference to a stack variable in `registerGameplay` will outlive that variable. The [timed-despawn example](examples.md#spawn-and-destroy-on-safe-boundaries) uses shared ownership for captured state and removes each object's entry on destruction.

## One fixed tick

The default interval is 1/60 second. A rendered frame may contain zero, one, or several fixed ticks. For each tick the runtime:

1. Applies structural commands queued by earlier work, in FIFO order.
2. Makes tick input available and calls `fixedUpdate`.
3. Steps the scene's Box2D or Box3D world with its configured substeps.
4. Reads physical poses back and delivers collision events to `onCollision` callbacks.

`spawn`, `destroy`, `addComponent`, and `removeComponent` queue structural changes. A command queued while this barrier or a callback runs waits until the **next** tick. This avoids invalidating the entity collection currently being visited. Runtime structural commands do not create an Editor Undo action or modify a saved scene.

A velocity is metres per second: assign it directly. A manually calculated displacement is speed multiplied by `delta`. The [physics controller](physics.md) demonstrates this distinction.

## One rendered frame

After its fixed ticks, the runtime calls `update` once. It then prepares presentation transforms, calls `lateUpdate`, and makes the final snapshot available to rendering.

For interpolation, the runtime blends the previous and current completed simulation poses using the accumulator fraction. Position and scale are interpolated linearly; rotation follows the shortest quaternion path. This normally displays a pose up to one fixed tick behind the latest simulated state. It is not prediction of a future physics pose.

Use `presentation(target)` in `lateUpdate` when a camera follows a physical object. Following `transform(target)` instead would follow the discrete simulation pose and can cause visible judder. Use `setPresentation` for the camera's visual pose; this method is permitted only during `lateUpdate` and does not write back into physics.

A non-physical pose changed in `update` is presented directly for that frame. Physical bodies reject ordinary `setTransform`; use an explicit teleport when discontinuous motion is intended. Spawn, teleport, scene replacement, and pause transitions reset the relevant interpolation history.

## Input, pause, and overload

`input()` supplies held horizontal/vertical axes and one-shot jump/interact edges. In `fixedUpdate`, an edge survives a rendered frame with no fixed tick and is consumed once, even when the next frame catches up several ticks. In `update`, input is the current frame's input. Consume a gameplay action in one chosen phase so your own code does not apply it twice.

The default catch-up limit is four ticks per frame. Excess whole intervals are dropped and reported as `dropped_time`; the fractional remainder is retained. Physics `delta` is not enlarged to compensate. This is a local-game policy, not a lockstep or rollback guarantee.

Player keys `P` and `N` pause and single-step. Pause clears accumulated wall time. One step advances one fixed tick and produces a current presentation pose. Resuming does not simulate the time spent paused.

## Configure simulation

The Player reads this optional object from the scene document. The settings are used by ordinary Play and `--validate`:

```json
{
  "simulation": {
    "fixed_delta": 0.016666666666666666,
    "max_catch_up_ticks": 4,
    "physics_substeps": 4,
    "gravity": [0, -9.81, 0]
  }
}
```

This is an excerpt, not a complete scene. The tutorial scenes contain complete examples. `fixed_delta` is seconds, gravity is metres per second squared, and substeps are solver subdivisions inside one fixed tick. These do not create additional gameplay callbacks. Invalid configuration is rejected before the Player starts. Editing these JSON settings is implemented; an Editor settings panel should only be relied on where the current UI exposes it.
