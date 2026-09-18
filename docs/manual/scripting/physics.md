# Move and jump with physics

A physical object's final pose belongs to Box2D or Box3D. Your behavior supplies intent through velocity, impulse, or an explicit teleport. It must not write a presentation position back into a rigid body each frame.

## Run the controller

Select `examples/tutorials/physics` as `FASET_GAMEPLAY_SOURCE_DIR`, build `faset_player`, and launch it with `--scene examples/tutorials/physics/scene.json`. Use the commands from [the first tutorial](first-behavior.md), changing the folder and build directory.

Press **A/D** or the left/right arrows to move, and **Space** to jump. The built-in Player supplies W/S as a vertical input axis too; this particular 2D controller deliberately uses only the horizontal axis. `P` pauses, `N` advances one tick while paused, and Escape closes the Player.

## Complete controller code

```cpp
--8<-- "examples/tutorials/physics/Gameplay.cpp"
```

The callback starts with the current velocity so it preserves the solver's vertical motion. It replaces only the horizontal component. An accepted jump replaces vertical velocity with `jump_speed`.

Do **not** multiply the assigned velocity by `delta`. The solver integrates metres per second over the fixed interval. `applyImpulse` is different: it applies a momentum impulse and its effect depends on mass. The adapter uses the body's centre and wakes it.

The collision callback is delivered after the physics step on the runtime's owning thread. It receives copied handles and a begin/end flag, not pointers into the native solver. This example prints a message when contact begins. Both objects can receive their own callback if both have registered behaviors.

## What grounded means

`grounded(self)` examines contact manifolds from the last completed physics step. A contact counts as support when its normal points sufficiently against gravity (dot product greater than 0.6) and at least one contact point is within 0.02 metres. With zero gravity, the query uses Y-up. The query supports both physics adapters.

This distinguishes a floor from a wall and from the top of a jump. **Zero vertical speed is not a ground test**: vertical speed is also near zero at the apex. The tests explicitly try to jump there and check that another upward impulse is not created.

New bodies have no support result before a physics step. Teleporting invalidates the old contact result until the next step. The query is a small support test; it is not a full character motor with step climbing, coyote time, jump buffering, moving-platform attachment, or a capsule controller.

## Scene components and units

The controller's scene includes a static floor and a dynamic box:

```json
--8<-- "examples/tutorials/physics/scene.json"
```

Use `faset.rigid_body_2d` in a 2D scene and `faset.rigid_body_3d` in a 3D scene. Each currently creates a box collider. `body_type` accepts `static`, `dynamic`, or `kinematic`. `half_extents` contains half the box dimensions in metres: two values for 2D, three for 3D. The object's absolute scale multiplies those extents at creation.

Density must be positive; friction is nonnegative; restitution is between zero and one. `linear_velocity` uses metres per second, `gravity_scale` scales world gravity for that body, and `category_bits`/`mask_bits` filter collisions. Rotation uses radians; a 2D rigid body rotates only around Z.

Initial adapters require physical bodies to be **root scene objects**. A parent transform is not silently baked into a rigid body's simulation frame. Collider scale cannot be changed through teleport; remove/re-add the body through the deferred component API when rebuilding its shape. More collider types and articulated character motors are separate work.

## Teleport and failure handling

For an intentional discontinuity, get a pose copy, change its position, and call `teleport(self, pose)`. It resets interpolation and wakes the body. It preserves the body's velocity; call `setVelocity(self, {0, 0, 0})` as well when resetting motion is intended.

A missing body or stale handle makes the physics accessor throw. Exceptions raised inside gameplay callbacks are recorded in `Runtime::diagnostics()` and printed by the Player; later phases continue. Fix the error instead of using exceptions as a normal ground test. A behavior that controls physics should be attached only to an object with the matching rigid-body component.
