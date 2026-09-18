# Faset Engine Manual

Faset is a C++ engine for desktop 2D and 3D games on Linux and Windows.
This manual focuses on writing gameplay: small working examples, the functions they use,
and how those functions interact with scenes, physics, and the editor.

!!! note "MVP scope"
    The C++ MVP includes the Editor, compiled gameplay tutorials, two playable games,
    Blender import and standalone Linux/Windows export. Acceptance used a physical
    Linux GPU and software Vulkan on Windows. See the
    [acceptance dossier](https://github.com/emil28092005/Faset_Engine/blob/main/docs/validation/mvp-acceptance.md)
    for exact source revisions and coverage limits. The optional Lua module is
    documented separately; those historical acceptance results do not certify later
    changes. Advanced graphics remain later milestones.

Start with [how gameplay works](scripting/index.md), then read
[frame and physics updates](scripting/lifecycle.md). See
[Build from source](getting-started/build.md) for the toolchain and build commands.

The engine, editor, built-in diagnostics, and API identifiers use English.
Your game content and project text can use other languages.

## Learning path

The manual grows alongside tested engine capabilities, in this order:

1. Build and run an example game.
2. Create a C++ behavior and expose a property in the Inspector.
3. Handle input and move a character.
4. Use physics, collision events, and deferred object creation.
5. Work with scene templates, assets, and references.
6. Import from Blender and export a standalone game.

For interpreted gameplay, follow [Lua gameplay](scripting/lua.md): declare component
fields, write callbacks, and reload scripts during development Play. The Lua-only
example in `examples/lua` uses the same physics and scene model as the C++ tutorials.

## Preview this manual

From the repository root, create a Python virtual environment and install the pinned documentation tools:

```sh
python3 -m venv .cache/docs-venv
.cache/docs-venv/bin/python -m pip install -r docs/requirements.txt
.cache/docs-venv/bin/python -m mkdocs serve
```

On Windows, use `py -m venv .cache/docs-venv` and
`.cache/docs-venv/Scripts/python.exe` in place of the Unix interpreter path.
The manual also remains readable directly as Markdown in the repository.
