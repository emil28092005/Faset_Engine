# Native Editor extensions

Editor extensions are startup-loaded `.so`/`.dll` modules. They are trusted native
code inside the Editor process. Gameplay remains statically linked into the separate
Player; an Editor extension is never required by the exported game.

The initial SDK registers commands and small action panels. Inspector fields for
runtime components come from the separate gameplay SchemaExporter. Rich custom
widgets and a general marketplace/package manager are later work.

## Example: Beacon

`examples/extensions/beacon` contains:

- `Beacon.hpp`: a runtime component schema and rotating-object behavior.
- `Editor.cpp`: an Editor command and a panel that creates a Beacon in one transaction.

Include `Beacon.hpp` from your project's `Scripts/Gameplay.cpp`. Call
`beacon::register_behavior(runtime)` from `registerGameplay`, and append
`beacon::schema()` to the array returned by `schema()`. Build gameplay so the Editor
can load the new metadata. Read [the first behavior tutorial](../scripting/first-behavior.md)
for the complete gameplay registration convention.

The normal engine build produces `example-plugin/` inside its build directory.
Copy its `beacon.faset-plugin.json` and native library into your project's
`Plugins/` directory, then restart the Editor. The **Beacon tools** panel offers
**Add Beacon**. Its command is also discoverable through MCP as
`plugin_example_beacon_create` and requires a document ID.

Disabling the Editor extension removes its panel and command after restart. It does
not erase its saved component data. Removing the runtime registration leaves an
unknown component preserved by authoring; exporting that scene fails until the
runtime dependency is restored or the component is deliberately removed.

## Compatibility and ownership

A manifest includes module ID/version, `kind: editor`, API version, native library,
build fingerprint and dependencies with exact versions. The loader validates the
complete graph for missing dependencies and cycles before calling entry points.
A failed dependency prevents loading its dependents.

The fingerprint includes the SDK sources, dependency lock, platform, architecture,
compiler version, configuration and CRT settings. Rebuild a plugin for the exact
Editor SDK. Compatibility across arbitrary C++ builds is not promised. Reloading an
updated native module requires restarting the Editor.

`include/faset/editor/plugin_api.h` defines a small C interface. Borrowed UTF-8 JSON
strings are valid during a call; responses are copied through a receiving callback.
Each side frees its own allocations. Register only during startup and invoke the SDK
on the Editor thread. Commands must use their owning module's name prefix. Panels
can invoke owned commands; a `$document` argument resolves to the active authoring
document. No EnTT registry, live runtime world, or arbitrary C++ object pointer is
exposed as a scripting API.

The native plugin test loads the actual example module, creates a component,
checks Undo, runs its separate runtime behavior, unloads command registrations,
opens the saved scene without the package, and rejects incompatible/cyclic/missing
plugin dependencies.
