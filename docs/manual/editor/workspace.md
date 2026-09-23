# Editor workspace

The Editor follows the window's display scale. Text is rasterized at that scale;
panel dimensions are saved in logical units so the layout remains usable when
moving between displays. A scale change preserves focused text and cancels an
unfinished numeric, divider or gizmo drag without committing an edit.

The Editor edits saved scene data and previews it in a Vulkan viewport. **Play**
opens a separate Player process. Gameplay movement and spawned objects stay in that
process; stopping it leaves the authoring scene unchanged.

## Open or create a project

Launch `faset_editor` without arguments to open the project launcher.

1. Choose **Create project**, enter a name and a new or empty project directory,
   then select **2D** or **3D** for the initial scene.
2. Choose **Create project** to create the project files and open the Editor.
3. To return later, choose **Open project**, select its directory with **Browse**,
   and confirm **Open project**. The directory must contain `project.faset.json`.
   A recent-project entry fills the directory field; confirm to open it.

The directory browser has **Up**, **Home**, and folder rows. Choose the directory
before confirming the launcher. **Tab** moves focus, **Ctrl+Enter** confirms the
current launcher/browser action, and **Escape** cancels the browser or launcher.

In an open Editor, **File → Open / create another project** returns to the launcher.
If there are unsaved scenes, queued/running jobs, or a Player, a dialog offers
**Cancel** or **Switch project**. Cancel and save first if you want ordinary scene
files updated. Switching stops that session's jobs and Player; unsaved authoring
changes remain in recovery journals. Project switching is disabled when the Editor
was launched in MCP mode so the connection keeps a single project context.

For a direct launch, use `faset_editor --project /path/to/MyGame` on Linux or
`faset_editor.exe --project C:\Projects\MyGame` on Windows.

## Create and save a scene

Use **File → New 2D scene** or **New 3D scene**. The Scene panel offers **+ Object**,
**Cube**, and **Sprite**. Select an object in the tree or viewport to inspect it.

Choose **Save** or press **Ctrl+S**. A new scene opens the File menu's path field;
enter a project-relative path such as `Scenes/Room.scene.json`, then choose
**Save scene as**. Subsequent saves update that file. The title's `*` marks unsaved
changes. **Open project-relative scene** opens another scene by its project-relative
path; the Assets panel also has **Open Scene**.

Save As refuses to overwrite a different existing file. If the scene file changed
outside the Editor, ordinary Save reports a conflict instead of overwriting it.
Save your in-memory work to a new path and compare the two versions before replacing
anything.

## Navigate and transform

- **Right drag:** orbit the 3D view; pan in a 2D scene.
- **Middle drag:** pan. **Mouse wheel:** zoom.
- **F** or **Frame:** frame the selected object.
- **W / E / R:** choose Move / Rotate / Scale. The viewport also has named buttons.
- Drag a gizmo axis to preview a transform; release to commit one Undo operation.
  **Escape** cancels the drag.
- Hold **Ctrl** while dragging to snap: movement uses 0.25-unit increments,
  rotation uses 15-degree increments, and scale uses 0.25 increments.
- **Delete** removes a local object, or suppresses an inherited template object.
  **Edit → Duplicate selection** duplicates an ordinary local subtree.

Move uses world axes, including for parented objects. Rotate and Scale operate in
local space. The Inspector stores rotation in **radians**. Dragging one Scene-tree
object onto another reparents it while preserving its world pose when representable;
drop into the tree's empty area to return it to the root. Template boundaries impose
additional rules described in [Scene templates](templates.md). Physics bodies must
remain roots for the current runtime adapters.

These shortcuts apply when a text or numeric editor is not consuming the key.
The viewport camera is an editing camera; navigating it does not rewrite a scene
camera component.

## Edit components and undo

The Inspector uses the registered component schema to show numbers, vectors,
checkboxes, enum choices, and text. Use **+ Add Component** to attach a registered
type; the component header's **x** removes a locally owned component.

Type into a field and press **Enter**, or move focus to commit. Numeric fields also
support dragging. A committed field edit or completed drag is one scene Undo action;
preview changes are not separate history entries. **Escape** cancels an unfinished
field edit. **Tab / Shift+Tab** move between controls. Invalid numbers keep their
error state until corrected or cancelled.

Use **Undo / Redo** or **Ctrl+Z / Ctrl+Shift+Z** after finishing the active field.
While editing text, its own editing history can consume Undo. Scene history belongs
to the active document: returning from a source template and undoing in its containing
scene does not undo the source document's edits.

GUI actions and MCP use the same authoring command service. If another action changes
the document while a field or gizmo drag is in progress, the old revision is rejected.
Read the fresh value and retry; the stale edit does not silently replace the newer one.

## Refresh C++ metadata

The status message indicates stale gameplay metadata, for example after changing
gameplay sources or after a failed build. Hover over **Build** for details, then
choose **Build** and check **Jobs** and **Console**. A successful build and schema
export refresh the Inspector. A failed build retains the previous metadata and
reports the failure.

A missing schema or unsupported component version appears as read-only raw fields
with **Copy raw fields**. Restore a missing module to make its schema available.
For an older component, declare [data migration rules](../scripting/api.md#editor-data-migrations),
choose **Build**, then **Migrate to v…** in the Inspector. This is one undoable
authoring edit; save explicitly afterward. Inherited components show **Open source
to migrate** instead. Top-level local additions migrate in their owning instance.
Missing rules or conversion errors preserve the data and appear in Console.

Opening or recovering a scene never migrates it automatically. Future versions
stay opaque and cannot be downgraded. Review instance overrides separately when
changing the source field's units or meaning. See [Build, Play, and export](export.md)
for the C++ iteration loop.

## Project settings

Open **File → Project settings** to change the project name, initial **2D / 3D** type,
and start-scene path. Save a scene first, then choose it from the saved-scene list or
enter its project-relative path. Choose **Save project** to update
`project.faset.json` explicitly. A missing start scene is rejected and the dialog
stays open.

These settings are used when the project opens again; they do not convert the active
scene, switch its dimension, restart the Player, or add a scene Undo entry.
**Cancel** or **Escape** discards this form's draft. If another writer changes the
project settings while the dialog is open, Save reports a revision conflict and
keeps your draft. **Reload saved** deliberately discards it and reloads the latest
saved settings before you retry.

## Configure scene simulation

Choose **Simulation** in the toolbar. These settings belong to the **current scene**:
fixed tick rate in Hz, maximum catch-up ticks, physics substeps, and gravity XYZ.
Defaults are 60 Hz, 4 catch-up ticks, 4 substeps, and `(0, -9.81, 0)` gravity in metres
per second squared. Each committed change is one Undo action and is saved with the
scene.

The next **Play** snapshot uses the settings. Editing them does not reconfigure an
already running Player. The 2D adapter uses gravity X/Y and ignores Z. See
[Physics and grounded movement](../scripting/physics.md) before changing the tick rate
or substeps.

## Panels and commands

**Assets** lists project files and imported resources. **Console** shows recent
messages and diagnostics. **Jobs** shows build/import/export progress and cancellation.
**Conflicts** lists unresolved template records. Drag the bottom tabs to reorder them;
drag panel dividers to resize the Scene, Inspector, and bottom areas. These choices
are stored per project in `.faset/editor-layout.json`.

File rows put the filename before its directory; hover to see the full project-relative
path. Long generated hexadecimal filenames are shortened in the list, while the
path and underlying asset identity remain intact. The search field filters by the
full project-relative path.

The current layout supports these panel sizes and bottom-tab ordering. It does not
provide floating panels or multiple Editor windows. Theme and base layout JSON live
in the engine's `assets/ui/dark.json` and `assets/ui/editor-layout.json`. The Editor
checks their contents every 500 ms and applies valid changes without restarting.
Malformed changes retain the last working appearance and report an error in Console;
fix the files and the next successful check applies them. Focus and unfinished field
text are preserved.

Layout reload updates properties of existing widgets; retain their IDs, kinds, and
parents. It is not a way to add arbitrary controls or move them to new parents.
Theme-only edits preserve resized panel widths. Editing the base layout can replace
properties explicitly present there, including panel sizes; per-project docking
preferences are stored separately.

**Commands** or **Ctrl+P** opens the command palette. Filter by command name, select a
command, enter its JSON arguments, and choose **Run selected command**. Results appear
in Console. For example, `faset_schema_status` takes `{}`. This is the same editor
command surface described in [MCP and command line](mcp.md).

## Recover unsaved work

Committed authoring changes are written to `.faset/recovery/`. On startup,
**Unsaved authoring recovery** offers **Restore &lt;scene name&gt;** for dirty journals.
Restore brings the recovered scene into memory; inspect it and **Save** when ready.
It recovers scene data, not the full previous session's Undo history or Player state.

**Continue without restoring** closes the prompt without deleting its journals.
Later edits or saves of the same document can replace its recovery record, so restore
or copy a journal before continuing if you still need that draft. Uncommitted text
or gizmo previews are not recovery checkpoints.

If the scene file changed or disappeared since the journal was written, restoration
reports a disk conflict. Keep copies of the journal and current file, then reconcile
them explicitly; recovery does not automatically overwrite external changes.
