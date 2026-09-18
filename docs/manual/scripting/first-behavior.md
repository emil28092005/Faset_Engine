# Your first behavior

This example moves a sprite along the X axis at two metres per second. It has no rigid body: the behavior owns its simulation pose.

## Build and run the complete example

First complete [the build setup](../getting-started/build.md), including the renderer dependencies. From the repository root on Linux:

```bash
cmake -S . -B build/tutorial-moving -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DFASET_GAMEPLAY_SOURCE_DIR="$PWD/examples/tutorials/moving"
cmake --build build/tutorial-moving --target faset_player faset_schema_exporter
build/tutorial-moving/faset_schema_exporter --output build/tutorial-moving/schema.json
build/tutorial-moving/faset_player --scene examples/tutorials/moving/scene.json
```

On Windows, use a Developer shell with `clang-cl`, the Windows SDK and the documented dependencies. Use `clang-cl` for both compiler options and an absolute path for `FASET_GAMEPLAY_SOURCE_DIR`; run `faset_player.exe` from the selected build directory.

The same directory contract is used by a project's `Scripts` folder. These commands select a complete gameplay module; they do not add its behavior to an unrelated module automatically.

## The module interface

```cpp
--8<-- "examples/tutorials/moving/Gameplay.hpp"
```

The header declares the two entry functions. `Runtime` is the game world API; its implementation and EnTT storage remain inside the engine.

## The implementation

```cpp
--8<-- "examples/tutorials/moving/Gameplay.cpp"
```

Read the callback from top to bottom:

1. `game` is the current runtime, and `self` is the object carrying `tutorial.move_x`.
2. `delta` is this frame's elapsed time in **seconds**.
3. `settings.value("speed", 2.0f)` reads configuration and supplies a fallback if the field is absent.
4. `transform` gives a pose copy. Multiplying metres per second by seconds gives a displacement in metres.
5. `setTransform` publishes the changed non-physical pose.

The `[](...) { ... }` expression is a C++ lambda: a function stored in `Behavior::update`. Empty brackets mean it captures no local variables. `registerBehavior` takes ownership of the callback object. Register before calling `load`; registration while entities exist or a callback is running is rejected.

The `schema()` function describes editable configuration. It does not create a runtime object. `tutorial.move_x` is the stable `TypeId`; `speed` is a stable `FieldId` within that type. Keep these IDs when changing a display label. Changing a field's meaning or units needs an explicit data migration, not just a new label.

## Attach the behavior

The example scene is a complete, loadable document:

```json
--8<-- "examples/tutorials/moving/scene.json"
```

The sprite is visible because it has `faset.sprite`. It moves because it also has `tutorial.move_x`. The configuration field is `speed`; the type string must match the registration exactly. `rotation` uses radians, and the default coordinate system is Y-up.

After **Build C++** succeeds in the Editor, choose **Add component** in the Inspector
and select the registered type. Its schema supplies editable fields, defaults and
constraints. The direct Player command above is useful for testing the same behavior
independently of an editor session.

## Make a change and verify it

Change the scene's `speed` to `-2`: the object moves left. Change the C++ callback or schema: stop the Player, rebuild, regenerate the schema, then launch a new session. There is no automatic C++ hot reload.

The `tutorial_moving` CTest checks that both 30 Hz and 60 Hz frame sequences move the object two metres in one second. It checks the resulting pose, rather than only checking that the program starts.

Common mistakes are forgetting `setTransform` after editing the copy, writing the wrong component type string, and attaching a rigid body while still using `setTransform`. The last case produces a runtime diagnostic: physics owns that body's pose. Continue with [physics movement](physics.md) for the correct API.
