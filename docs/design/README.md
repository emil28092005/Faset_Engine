# Editor visual references

These image prototypes were generated from the recorded native editor screenshot in
`docs/validation/checkpoint5-linux-2026-09-18/editor-blender.png` before changing the
P1 interface. They are visual references for the existing compact dark treatment,
not specifications of behavior, layout measurements, copy, or platform paths.

- `p1-iteration-console-reference.png` explores structured build diagnostics,
  source navigation, a cache indicator, and autosave status in the existing editor.
- `p1-project-template-reference.png` explores a compact 2D/3D and C++/Lua project
  chooser. Its example Windows path is illustrative; the implementation must use
  native paths on Linux and Windows.

Both were created with the built-in image generation tool using the cited screenshot
as the edit reference. The accepted behavior and validation criteria are in the
[P1 design](../superpowers/specs/2026-09-24-p1-iteration-design.md) and
[`PLAN.md`](../../PLAN.md).

The [editor context-menu reference](editor-context-menu-reference.png) explores
the hierarchy, asset-browser, and viewport menus in the same dark editor style.
It is a **visual reference**, not the authority for enabled actions, wording,
positions, or behavior. The [Editor workspace manual](../manual/editor/workspace.md#right-click-context-menus)
describes the intended interaction; source code and tests determine the implemented
behavior.
The [Linux implementation capture](../validation/context-menus-2026-09-24/scene-context.png)
shows the Scene-root menu rendered by the acceptance test.
