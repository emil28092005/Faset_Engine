# Lua gameplay

Faset embeds **Lua 5.4.9** as an optional gameplay module. Lua and compiled C++
behaviors share the same runtime lifecycle, typed entity operations, scene components,
and Inspector metadata. The Editor does not run gameplay code in its own process.
There is no built-in script editor: edit `.lua` files in Zed or another external editor.

## Start with a runnable Lua project

Choose **Create project → Lua → 2D or 3D** in the native launcher, or run:

```sh
build/linux-debug/faset_editor --project "$PWD/LuaGame" --new LuaGame --dimension 2 --language lua
```

The starter creates `Scripts/main.lua`, a scene with a player and ground, and a
manifest entry for that script. The player moves horizontally and jumps with the
default input actions. The 3D choice also provides a camera and directional light.
Open `Scripts/main.lua` in your external editor and change `speed` or
`fixed_update`. **Refresh Lua** updates declared Inspector fields; **Reload Lua**
updates a development Player after a valid edit without rebuilding native gameplay.
The starter installs LuaLS declarations and `.luarc.json`; reopening it in Zed or
another LuaLS-enabled editor provides completions for the Faset API.

## Enable Lua in a project

Add explicit entry scripts to `project.faset.json`:

```json
"scripting": {
  "lua": {
    "scripts": ["Scripts/player.lua", "Scripts/beacon.lua"]
  }
}
```

This is a manifest fragment, not a complete project file. Each entry must return one
`faset.behavior` table with a unique custom TypeId. All sources live beneath `Scripts`
and are captured as an immutable build/export snapshot. Paths must be project-relative;
symlinks and paths outside `Scripts` are rejected. Auxiliary modules do not need to
appear in the entry list.

A Lua-only project can omit both `Scripts/Gameplay.cpp` and `Scripts/Gameplay.hpp`.
A mixed project keeps that pair and adds the Lua declaration. TypeIds must be unique
across both languages, and the `faset.*` namespace is reserved for native components.

The engine developer option `FASET_ENABLE_LUA` defaults to `ON`. Project builds select
it from the manifest, so a C++-only game does not link the Lua VM. Lua is pinned and
built from source; no system Lua installation is required.

The complete `examples/lua` project includes a playable
2D controller, a non-physical animated beacon, and a shared module. Its scripts are
also loaded by the Lua contract test.

## Write a behavior

```lua
local Player = faset.behavior {
    id = "game.player",
    version = 1,
    name = "Player",
    fields = {
        speed = {
            name = "Move speed", type = "number", default = 5,
            min = 0, max = 30, units = "m/s"
        }
    }
}

function Player:on_start()
    self.state.elapsed = 0
end

function Player:fixed_update(delta)
    self.state.elapsed = self.state.elapsed + delta
    local velocity = self.entity:velocity()
    velocity.x = faset.input().horizontal * self.fields.speed
    self.entity:set_velocity(velocity)
end

return Player
```

Attach a component with `type: "game.player"`, `version: 1`, and the desired field
overrides to an entity with a 2D or 3D rigid body. Refresh schemas to expose the
behavior in **Add Component** and its `speed` field in the Inspector. Saved scenes
store the stable TypeId and data, not an instance of a Lua object.

Each entity/component gets its own instance:

- `self.entity`: an opaque runtime handle, checked on every call.
- `self.fields`: a configuration copy, combining schema defaults and scene overrides.
- `self.state`: a fresh mutable table for counters, timers, and retained handles.

Changing either table does not modify the saved scene or create an Undo operation.
Module-local variables are shared by instances of that module; put per-entity state
in `self.state`. Lua tables returned by getters are copies, not native pointers.

## Lifecycle

Use colon definitions so Lua supplies `self`:

| Callback | When it runs |
|---|---|
| `on_start()` | Once after the instance and initial scene objects exist |
| `fixed_update(delta)` | Before each fixed physics step; delta is seconds |
| `on_collision(event)` | After physics, for contact begin/end |
| `update(delta)` | Once per rendered frame after fixed steps |
| `late_update(delta)` | After presentation interpolation |
| `on_destroy()` | Before component/entity removal, while the handle is still valid |

Omit unused callbacks. The same [timing rules](lifecycle.md) as C++ apply, including
input edges, fixed-tick catch-up, deferred structural changes, pause and single-step.
Do not multiply velocity by delta; multiply a manually calculated displacement.

An error is reported with source location/traceback and disables the offending
instance for that generation and releases its instance state. Other instances can continue.
The VM quota is shared: allocations retained by module-level variables can still
affect other behaviors. A restart/reload creates
fresh instances; disabled instances are not automatically retried every frame.
Changes already made or queued by a failing callback are not rolled back.

## Runtime API

`faset.find("scene-id")` returns an entity handle or `nil`. Handles support equality
and `:valid()`. Retained handles become invalid after destruction or scene restart;
calling other methods on a stale handle reports an error.

| Entity method | Contract |
|---|---|
| `:transform()` / `:presentation()` | Copy of simulation/display transform |
| `:set_transform(pose)` | Non-physical objects only |
| `:set_presentation(pose)` | Display-only write during `late_update` |
| `:teleport(pose)` | Explicit discontinuous pose change; preserves velocity |
| `:fields(type_id)` | Copy of the named component's stored fields |
| `:velocity()` / `:set_velocity(v)` | Linear velocity, rigid bodies only |
| `:apply_impulse(v)` | Impulse at the rigid body's centre |
| `:is_grounded()` | Support from completed native physics contacts |
| `:destroy()` | Queue entity/descendant removal |
| `:add_component(record)` | Queue a complete component record |
| `:remove_component(type_id)` | Queue component removal |

Typed vectors are `{x = 1, y = 2, z = 0}`. Transforms contain `position`, `rotation`,
and `scale`, each a named vector. Positions use metres; rotations use XYZ Euler
radians. In contrast, **scene/component JSON arrays** are represented as ordinary
1-based Lua arrays, such as `fields.position = {1, 2, 0}`. Use `faset.null` to retain
an explicit JSON null; Lua `nil` removes a table key.
An empty Lua table converts to a JSON object; an empty schema default with
`type = "array"` is normalized to an empty JSON array.

`faset.input()` returns `horizontal`, `vertical`, `jump_pressed`, and
`interact_pressed`. Player mappings are A/D or arrows, W/S or arrows, Space, and E.
`faset.log(...)` sends a bounded message to Player logs and the Editor Console.

Collision events contain `first`, `second`, `other` (the opposite entity), and `began`.
They are copied for Lua, but retained entity handles still need validity checks.

`faset.spawn(record)` queues a full scene entity record. It returns **no handle**:
use `faset.find(id)` after the next fixed-tick barrier. Spawn, destroy, add and remove
operations follow FIFO order and do not mutate Editor documents. For example:

```lua
faset.spawn {
    id = "effect-1", name = "Effect", parent = faset.null,
    components = {
        {
            id = "effect-transform", type = "faset.transform", version = 1,
            fields = { position = {0, 2, 0} }
        }
    }
}
```

## Shared modules and sandbox

`require("util.motion")` resolves `Scripts/util/motion.lua`, then
`Scripts/util/motion/init.lua`, inside the captured source snapshot. A module is
evaluated once and its result cached within the VM. Missing modules, cycles, and
path-like names are errors. There is no native module search, package installation,
network access, or arbitrary file access.

Basic Lua operations and the `math`, `string`, `table`, and `utf8` libraries are
available. `io`, `os`, `debug`, dynamic `load`, `loadfile`, `dofile`, `pcall`, `xpcall`,
`setmetatable`, `collectgarbage`, `string.dump`, and coroutines are not exposed.
Engine-owned metatables are locked; arbitrary finalizers cannot run during shutdown.
The restricted API intentionally prevents scripts
from catching execution-limit errors and continuing indefinitely.

The VM has memory and instruction budgets (`LuaLimits`, default 16 MiB and one
million instructions per protected entry/callback). These are gameplay reliability
limits, not a promise that executing untrusted code is equivalent to OS isolation.
JSON conversion also limits nesting, node count and expanded string/key bytes
(16 MiB), including repeated references to the same Lua string. Structural commands
are limited to 1,024 operations and 16 MiB of marshaled payload per callback.
The Player already runs separately from the Editor; only trusted local game projects
should be opened and built. C++ gameplay is native code and is not sandboxed.

Schema extraction evaluates entry scripts in the bounded VM but does not create a
world or invoke lifecycle callbacks. Keep top-level code declarative: calling runtime
operations there is an error. Metadata supports the same fields, constraints and
declarative [migration rules](api.md#editor-data-migrations) as C++ schemas. Runtime
loading does not migrate saved data automatically.

## Edit, reload, and export

The Editor command palette exposes:

| Command | Purpose |
|---|---|
| `faset_lua_refresh` | Build if needed, extract schemas, refresh Inspector metadata |
| `faset_lua_reload` | Request a Lua reload in a development Player |
| `faset_lua_setup` | Install Faset LuaLS declarations/configuration |
| `faset_script_open` | Open a script in an external editor |

The external-editor default is `zed`. Set `editor.script_editor` in
`project.faset.json` to an argument array such as `["code", "--goto", "{file}"]`,
or pass an `editor` argument array to `faset_script_open`. Exact `{file}` and
`{project}` arguments are substituted; a missing file argument is appended. The
command launches the executable directly, without a shell. The Assets panel lists
Lua sources under `Scripts` and provides **Open Script**.

Development Play watches Lua changes. The Player's `--watch-lua` option enables this
for direct development runs. A candidate source generation is loaded and validated
before replacement; an invalid candidate leaves the preceding generation running.
Successful reload **restarts the scene**, invalidates old handles, and resets all
script state. This is not state-preserving hot swapping. C++ source changes still
require a rebuild and a new Player process.

Export captures the declared entry list and Lua modules with the game. The exported
Player runs without the Editor or a separate Lua installation; development watching
is not enabled by ordinary exported-game launch. Exported Lua remains readable source,
not encrypted code. A C++-only project continues to export without the Lua VM.

## Zed and LuaLS

Run `faset_lua_setup`. It copies annotation-only declarations to
`.faset/lua/faset.lua` and creates `.luarc.json` **only if it does not already exist**.
It also creates `Scripts/.luarc.json` for editors that open an individual Lua file
with `Scripts` as the workspace root. Existing configuration files are preserved.
For an existing LuaLS configuration, merge these settings yourself:

```json
{
  "runtime.version": "Lua 5.4",
  "runtime.path": ["Scripts/?.lua", "Scripts/?/init.lua"],
  "workspace.library": [".faset/lua"],
  "workspace.checkThirdParty": false,
  "diagnostics.globals": ["faset"]
}
```

Use an editor with LuaLS integration and open the project directory. Annotations
describe the Faset API for completion and diagnostics; they are not runtime code and
must not be `require`d. The engine does not embed an LSP client, code editor, or a
breakpoint debugger. Player logs/tracebacks are the first debugging surface.
