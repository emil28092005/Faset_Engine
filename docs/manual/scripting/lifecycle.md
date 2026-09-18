# Frame and physics updates

!!! note "Execution contract"
    This page describes the accepted runtime contract. The runnable callback examples
    and test results are added as the runtime implementation becomes available.

## Choose the right callback

- `OnStart`: initialize a behavior once its object and components exist.
- `FixedUpdate`: update simulation logic before a physics step.
- `Update`: run frame-based gameplay once per rendered frame.
- `LateUpdate`: update cameras and dependent visual objects after presentation interpolation.
- `OnDestroy`: release subscriptions and other behavior-owned state before its handle is invalidated.

The default simulation interval is 1/60 second. A rendered frame may contain zero,
one, or several fixed ticks. Frame rate and physics rate are not the same quantity.

## Fixed tick order

1. Apply structural commands queued by earlier work.
2. Deliver tick input and call `FixedUpdate`.
3. Apply physics commands and step the 2D and 3D worlds.
4. Read back transforms and queue collision events.
5. Run reactions after physics.

Object creation/removal and component addition/removal are deferred to the beginning
of the next fixed tick. This prevents a callback from invalidating the collection
currently being processed. New objects follow the same initialization rules as objects
loaded from a scene.

After the fixed ticks, the frame runs `Update`, prepares interpolated presentation
transforms, calls `LateUpdate`, and produces the render snapshot.

## Avoid frame-rate-dependent movement

A speed is a distance per second. Multiply it by the callback's elapsed seconds when
calculating a displacement. Do not multiply a velocity by elapsed time before assigning
it to a physics velocity API; the physics step performs that integration.

## Overload and pause

The initial catch-up limit is four fixed ticks per frame. Excess whole intervals are
dropped with a diagnostic rather than making the physics step arbitrarily large.
This is a local-game policy, not a guarantee of deterministic network simulation.

Pausing clears accumulated time. Single-step advances exactly one simulation tick.
Interpolation history is reset for a new session, spawn, or teleport.
