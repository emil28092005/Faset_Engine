# Scene templates and local overrides

A template is an ordinary `.scene.json` scene used as the source of an instance in
another scene. Each instance stores its source path and local differences. Editing
a source field updates instances that have not overridden that field. Templates can
contain other scene instances, forming a nested composition.

## Make a reusable object

1. Create an object and its children, then select the root object in the Scene tree.
2. Open **Scene** and set the template path, for example
   `Assets/Templates/Door.scene.json`.
3. Choose **Save selection as template**.
4. Select a containing scene, open **Scene**, enter the same source path, and choose
   **Instance scene at this path**. Alternatively, select the saved scene in Assets
   and choose **Instance Scene**.

The save operation copies the selected **resolved subtree** to a new source file.
It does not replace the original objects with an instance or apply changes to an
existing source. Nested content inside that copied subtree is flattened into ordinary
objects. The selected root becomes parentless and retains its local transform, so
check its placement if it was previously under a transformed parent. References to
objects outside the copied subtree need deliberate handling; they are not collected
automatically.

Use a new path: Save As will not overwrite another existing scene file. Creating this
source file is separate from the containing scene's Undo history. Remove the original
objects only if you intend to replace them with your new instance.

## Recognize an instance

The Scene tree shows a **[T]** group named after the source scene. Nested instances
have their own indented groups. Select the group to see **Scene instance** in the
Inspector, including its source path and **Open source**.

Select an inherited object inside the group to edit its fields. Its Inspector shows
the source filename. Each exposed field is marked **Source** when inherited or
**Override** when the containing scene supplies a local value.

To change one door's position or behavior setting, edit that field in the instance.
This records a local override; it does not modify the source file. **Revert** removes
that field's local override so it follows the source again. Undo can restore the
previous override.

## Edit the source

Choose **Open source** from an instance or inherited-object Inspector. Edit the
opened source scene, then use the viewport's **Back** button to return to the previous
document. Back switches documents; it is not an Undo operation.

The current editor session resolves instances against an open source document's
in-memory state, including unsaved edits. Save each changed source scene to make those
edits available after restarting or to another session. Saving only the containing
scene does not save its source documents.

Renaming, adding/removing source components, and deeper nested structural changes are
done in the appropriate source document. Inherited object names and component
structure are not editable as arbitrary local overrides. There is no **Apply all
instance changes to source** action in this version: open the source and make the
intended shared edit explicitly.

## Make structural changes to one instance

The top-level instance Inspector provides these operations:

- **Add local object** creates an object owned by this instance. An inherited object's
  **Add local child** creates one parented to that object. Local additions can be
  renamed and have components added or removed without changing the source.
- **Delete** on an inherited object suppresses it in this instance. The source stays
  intact. Select the instance group and choose its **Restore suppressed object …**
  button to remove that suppression.
- **Remove instance** removes the entire top-level instance record from the containing
  scene. It does not delete the source file. Undo restores the record and its local
  differences.
- Edit the top-level instance's **source path** to point to another scene. Existing
  local differences are retained; review Conflicts because the new source may not
  contain their targets.

These edits are authoring transactions and support Undo. Removing a local addition
from the visible instance also uses suppression, so the instance Inspector can restore
it.

Drag an inherited object's tree row onto another object **within the same instance
path** to reparent it locally. The editor keeps its world pose when representable.
Dragging across instance boundaries is rejected; use **Add local child** or edit the
source hierarchy instead. A nested instance group disables top-level structural
controls and directs you to **Open source**.

## Resolve conflicts without losing overrides

An override targets source object, component, and field IDs rather than the label
shown in the tree. If the source component is removed, for example, the local override
cannot be applied. The Editor keeps that record and lists it in **Conflicts** instead
of silently discarding it.

For a reproducible example:

1. Add a component to the source and save it.
2. In the containing scene, override one of that component's fields.
3. Open the source and remove that component.
4. Return with **Back**. The containing scene reports the unresolved override.

Use the conflict's **Open source** button to inspect the change. If the source deletion
was accidental, undo it in the source document; restoring the original identity lets
the override resolve again. Recreating a same-named component can give it a new ID and
does not automatically reconnect the old record.

If the local value is no longer needed, **Discard override** removes that record from
the containing scene. This action supports Undo. A missing suppressed object can also
show **Discard suppression**. These discard buttons cover supported top-level records;
for nested-source conflicts, open the source that owns the change. Other conflict
kinds are displayed for diagnosis and are not automatically repaired by renaming.

Resolve conflicts before Play or export. A missing source, source cycle, or invalid
address is not a successful partial game build. See [Build, Play, and export](export.md)
for validation and job diagnostics.

## Scope and persistence

Save the containing scene to persist its instance paths, overrides, suppressions,
local additions, and reparents. Save source scenes separately. Unsaved committed
changes have the same [recovery behavior](workspace.md#recover-unsaved-work) as other
authoring edits.

Template resolution produces the flattened scene used for preview and Player startup.
The running game's components are independent of this authoring composition: gameplay
writes do not become template overrides, and MCP does not inspect or change the
Player's live entities.
