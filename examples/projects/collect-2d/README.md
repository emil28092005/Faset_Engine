# Collect & Escape 2D

A small playable project using the real Faset C++ gameplay module and Box2D adapter. All geometry is built in, so this project does not need an asset import or Blender installation.

## Play

Open it from the repository root with a built Editor:

```bash
build/linux-debug/faset_editor --project examples/projects/collect-2d
```

Use **Play** to build this project's `Scripts/Gameplay.cpp` and start a separate Player. On Windows use your Windows build directory and `faset_editor.exe`. First complete the engine build setup in the User Manual.

For a direct Player build on Linux, from the repository root:

```bash
cmake -S . -B build/collect-2d -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DFASET_GAMEPLAY_SOURCE_DIR="$PWD/examples/projects/collect-2d/Scripts"
cmake --build build/collect-2d --target faset_player faset_schema_exporter
build/collect-2d/faset_player --scene examples/projects/collect-2d/Scenes/main.scene.json
```

On Windows use a Developer shell with `clang-cl` and the Windows SDK, select `clang-cl` for both compiler options, and give `FASET_GAMEPLAY_SOURCE_DIR` an absolute path.

## Objective and controls

- A/D or left/right arrows move on X.
- **Space** jumps while a native contact supports the player block.
- Collect all three **gold cubes**. Jump onto the raised platform for the middle pickup.
- Each pickup moves to the progress display. After all three, the **red physical gate** moves out of the level.
- Reach the **green exit** to reveal the large gold victory marker. Movement stops after completion.
- **E** restores the player, pickups, gate, and victory state. Falling below the level also resets the round.
- **P** pauses, **N** advances one fixed tick, and **Escape** closes the Player.

The Player window title and console identify the controls and objective; the game uses geometric progress markers rather than a text HUD. The player is a dynamic box with zero friction, not an articulated character controller. It can rotate after impacts.

## Inspect and change the game

`project.faset.json` chooses `Scenes/main.scene.json`. The scene stores object/component IDs, transforms, physics settings, and entity references in the collector component. `Scripts/Gameplay.cpp` owns the round's transient C++ state and registers its editable schema. `speed` and `jump_speed` use metres per second.

Edit the scene through the Inspector and restart Play to see new level data. Change C++ or its schema, then stop, build, and restart; C++ hot reload is not implemented. Runtime pickup progress does not rewrite the authoring scene or create Undo entries.

## Verify

The normal test build creates `faset_example_2d_tests`. It compiles this exact module, follows a route using movement/jump input, collects the elevated pickup, opens the gate, reaches the exit, resets the round, and restarts the scene. It does not teleport to pass the objective.

```bash
cmake --build build/linux-debug --target faset_example_2d_tests
ctest --test-dir build/linux-debug -R '^playable_2d$' --output-on-failure
```

The runtime test is independent of graphics. A Player run additionally requires the supported Vulkan platform setup. The Editor's Export action creates a separate Release build and a cooked standalone generation; run that generation's Player from its export directory.
