# More complete examples

Each folder below is a separate, buildable gameplay module with `Gameplay.hpp`, `Gameplay.cpp`, and `scene.json`. Select one using `FASET_GAMEPLAY_SOURCE_DIR`, as shown in [the first tutorial](first-behavior.md). The Player links that module statically.

## Follow an interpolated object

`examples/tutorials/following` moves an object during fixed updates. A camera follows its presentation pose during `lateUpdate`:

```cpp
--8<-- "examples/tutorials/following/Gameplay.cpp"
```

The camera resolves the saved target ID on each frame, handles its disappearance, reads `presentation(target)`, and writes only its own presentation pose. In this small scene both objects are roots, so their coordinate frames match. For objects under different parents, transform between coordinate frames explicitly; adding local positions from unrelated parents is incorrect.

The `tutorial_following` test advances one and a half fixed intervals. It checks that the camera follows the halfway presentation position, while its simulation pose is unchanged. It also removes the target to exercise missing-handle behavior.

## Spawn and destroy on safe boundaries

`examples/tutorials/spawning` creates a temporary sprite and removes it after its lifetime:

```cpp
--8<-- "examples/tutorials/spawning/Gameplay.cpp"
```

The spawner's `onStart` queues creation. The new object's `onStart` runs only when that command is applied. Its elapsed time is ordinary C++ state owned by the callbacks. A key contains all three handle fields, so a recycled slot or restarted session cannot accidentally reuse an older object's timer.

When the timer expires, `destroy` queues removal; the handle remains valid until the next barrier. `onDestroy` removes the stored timer entry. The example's `spawned_id` must be unique in the runtime scene: using the same value on several spawners produces a duplicate-ID diagnostic. A production spawner should choose an appropriate runtime ID policy.

This does not save the spawned object into the authoring document. The test checks deferred creation, eventual invalidation, and a fresh spawn after reloading the scene.

## Use a contact-based jump

`examples/tutorials/physics` contains the [complete physics controller](physics.md). Its test checks movement, a supported jump, rejection of a jump at the apex, and landing. The default example gameplay module also uses the same grounded query.

The query's slope/separation thresholds are intentionally small and explicit. Extend the gameplay controller when your game needs coyote time, jump buffering, climbing steps, or moving platforms; these are not automatically provided by naming a component “character”.

## Run the tutorial checks

After configuring a normal build with `BUILD_TESTING=ON`:

```bash
cmake --build build/linux-debug --target \
  faset_tutorial_moving_tests faset_tutorial_following_tests \
  faset_tutorial_spawning_tests faset_tutorial_physics_tests
ctest --test-dir build/linux-debug -R '^tutorial_' --output-on-failure
```

On Windows use the chosen Windows build directory. The four tests compile separate gameplay libraries and execute their actual callbacks. They require neither a graphics window nor the Editor. Running a tutorial through `faset_player` additionally checks rendering and platform integration and requires the documented Vulkan setup.

Schema declarations are tested for stable map-key/FieldId matching and defaults. They remain ordinary C++ source; the engine does not scan arbitrary C++ classes to create this API automatically.
