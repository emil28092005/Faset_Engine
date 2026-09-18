# Faset Engine Manual

Faset is a C++ engine for desktop 2D and 3D games on Linux and Windows.
This manual focuses on writing gameplay: small working examples, the functions they use,
and how those functions interact with scenes, physics, and the editor.

!!! warning "Development status"
    MVP implementation is in progress. A planned feature is not a working feature.
    Individual guides state their prerequisites and validation status. The current
    Editor, gameplay tutorials and Linux export can be built and tested. Final Windows graphics/export acceptance and complete sample games are still in progress.

Start with [how C++ gameplay works](scripting/index.md), then read
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

Lua is planned after the C++ foundation. It is not a current scripting option.

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
