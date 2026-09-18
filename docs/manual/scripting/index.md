# Gameplay scripting

Faset supports **C++ compiled into the Player** and optional [Lua gameplay](lua.md).
Both languages register component schemas and callbacks on the same runtime.
For C++, there is no interpreter or live replacement of compiled classes: stop Play,
rebuild, export the schema, and start a new Player session.

The following tutorials describe C++. See [Lua gameplay](lua.md) for Lua-only or
mixed projects, the Lua API, source reload, and external-editor completion.

## Start here

1. Read [Your first behavior](first-behavior.md) and run the moving-object example.
2. Learn [when callbacks run](lifecycle.md) before mixing frame updates and physics.
3. Build a [physics character](physics.md) that can move and jump from the floor.
4. Try [following, spawning, and timed destruction](examples.md).
5. Keep the [runtime API reference](api.md) nearby while writing code.

The complete tutorial modules are compiled and executed by CTest. The code blocks include those source files directly, so the manual does not maintain separate, untested copies.

## What belongs to your module

A gameplay directory contains `Gameplay.hpp` and `Gameplay.cpp`. It provides two functions in `faset::gameplay`:

- `registerGameplay(runtime::Runtime&)` registers executable behavior callbacks.
- `schema()` returns a JSON array of component descriptions: stable type and field IDs, versions, defaults, constraints, and Inspector hints.

The Player calls registration before loading a scene. The separate SchemaExporter calls `schema()` without creating a game world or running gameplay callbacks. The Editor reads the resulting declaration; it does not load the gameplay binary into the Editor process.

Every custom component in a played scene must have a matching TypeId and exact
positive version in the linked gameplay `schema()`. The Player checks this before
loading the world, including `--validate`. A data-only custom component still needs
a schema, even when it registers no callbacks. Built-in TypeIds are reserved. The
Editor preserves missing or future component data for recovery, but the Player
rejects it until the corresponding module/schema is available or the data is migrated.

A scene object receives a behavior by containing a component whose `type` matches the string passed to `registerBehavior`. Registering a behavior does not attach it to every object. A component can contain data without having any callbacks.

## Data, poses, and state

`fields(self, type)` returns a **copy of component configuration**. Editing that copy changes neither the saved scene nor the runtime configuration. `transform(self)` returns a copy of the current simulation pose; pass the changed copy to `setTransform` for a non-physical object. A rigid body uses `setVelocity`, `applyImpulse`, or an explicit `teleport` instead.

Ordinary C++ state can be captured by callbacks. The spawning tutorial shows state indexed by the full runtime handle and cleaned up in `onDestroy`. Do not capture a reference to a local variable that will disappear after `registerGameplay` returns. Use owned state, or ensure the referenced object outlives the runtime.

Scene IDs are saved strings. `EntityHandle` is a temporary reference containing a session, slot, and generation. It must not be written into a save file. Resolve a scene ID with `find`, check `valid`, and expect old handles to stop working after removal or a new Play session.

## Current boundaries

These examples use per-object callbacks and the implemented typed pose/physics API. They do not provide a universal binding for arbitrary C++ classes, a public EnTT registry, or a general parallel-system scheduler. The runtime is sequential and has one owning thread.

MCP belongs to the Editor's authoring, import, build, and process-control services. It does not invoke runtime methods or inspect the live game world. A C++ gameplay change requires the same rebuild whether a person or an agent edited the source.
