# Lua playground

A Lua-only project: there is no project `Gameplay.cpp`. The native engine and Player
are built normally; the two gameplay behaviors are loaded from Lua source.

Open this folder as a project in the Editor, refresh Lua schemas, open
`Scenes/main.scene.json`, then Play. **A/D** or arrows move, **Space** jumps from
ground, and **E** resets the player. The gold beacon shows non-physical animation
and a shared `require("util.motion")` module.

Change `Scripts/player.lua` in an external editor and save. Development Play watches
Lua sources; a successful reload restarts the scene and resets script state. Invalid
source leaves the previous generation running and reports the error. Export copies
the captured Lua files into the game; a Lua executable or separately installed Lua
library is not needed.

Run `faset_lua_setup` through the Editor command palette to install the Faset LuaLS
declarations and, if absent, `.luarc.json`. See the
[Lua manual](../../docs/manual/scripting/lua.md) for the API and sandbox boundaries.
