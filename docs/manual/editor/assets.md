# Assets and Blender

Keep source assets under your project's `Assets` directory. Faset imports PNG/JPEG,
static glTF/GLB, and manifests published by the optional Blender helper. The import
service runs in the Editor; exported games load cooked data and need no Blender.

## Import a model

1. Copy a `.glb`, or a `.gltf` with its relative buffers and images, into `Assets`.
2. In the **Assets** panel select the source and use **Import**. Watch the job state
   and diagnostics. Import is asynchronous; the previous working generation stays
   available until a complete replacement is ready.
3. Place the imported scene in the viewport. The Faset object stores the AssetId;
   its transform, gameplay components, and physics remain authoring data.
4. Add a rigid body explicitly if needed. Import does not infer gameplay collisions
   from the visual mesh.

The same operation works through the command line:

```sh
build/linux-debug/faset_editor --project MyGame --command \
  '{"name":"faset_import","arguments":{"path":"Assets/door.glb"}}' --wait
```

With MCP, call `faset_import`, then query `faset_job` using the returned job ID.
`faset_job_cancel` requests cancellation. Cancelling an import does not Undo an
authoring edit. See [MCP and CLI](mcp.md) for transport setup and errors.

The Assets panel marks an imported asset **Stale** when its source, external glTF
buffer/image, Blender bundle, saved import settings, importer, or target profile
differs from the active generation. Select the imported row, then **Import / Reimport**;
the Console reports the changed input. **Refresh** checks immediately, and the panel
also refreshes periodically. `faset_assets` exposes the same `freshness.state` and
structured `freshness.reasons` to MCP clients. Checks compare contents, including
files whose size and timestamp stayed unchanged. The last good cooked asset remains
visible until reimport succeeds; checking freshness never publishes a generation.
Cook and export refuse referenced stale or unavailable sources and explain which
input needs reimport. A standalone exported game uses only its packaged cooked
generation and never needs the original source files.

## Import an image

PNG and JPEG become image assets. Drag an imported image into a scene to create a
sprite. `pixels_per_unit` is a positive import setting, defaulting to 100; it controls
the sprite's natural size. Color, transform, and layer remain scene properties.

```json
{"name":"faset_import","arguments":{
  "path":"Assets/player.png","settings":{"pixels_per_unit":32}
}}
```

Omitting `settings` reuses the saved recipe. Providing an object replaces the recipe;
it is not a partial merge. Malformed or oversized images produce diagnostics and
retain the last successful generation.

## Use the optional Blender helper

The helper works in unmodified Blender. Blender 4.5.3 LTS is the version used for
the observed round-trip test. Ordinary GLB import remains available without it.

1. Zip the repository's `tools/blender_addon` directory with that directory at the
   ZIP's top level. Install the ZIP through Blender's add-on preferences and enable
   **Faset GLB Export**.
2. Build a static scene using ordinary meshes and glTF-compatible PBR materials.
3. Choose **File → Export → Faset GLB Bundle** and place `manifest.json` in a directory
   inside the Faset project's `Assets` directory.
4. **Save the `.blend` after the first export.** The helper assigns `faset_id` custom
   properties to objects, meshes and materials, and `faset_asset_id` to the scene.
5. Import the exported `manifest.json` in Faset. Keep the manifest and its
   `payload/<sha256>.glb` files together.

The helper publishes the manifest only after the payload is complete. It exports
the active Blender scene, not just the current selection. Gameplay and physics
components are added in Faset and are never written by the Blender helper.

## Rename, duplicate, and reimport

Renaming an object with a saved `faset_id` preserves its source identity. Re-export
and reimport update the shared asset; scene placements using that AssetId load the
new geometry while keeping their own transforms and components.

Duplicating an object can also duplicate its custom ID. The exporter rejects
ambiguous IDs. Select the new copy and run **Faset: New IDs for Selected**. Shared
mesh datablocks remain shared. Repair deliberately duplicated material IDs in
Blender's Custom Properties. Save the `.blend` again.

Removing an exported output produces an import conflict. Faset keeps the old
generation active. In **Jobs**, choose **Review removals** to open **Conflicts**.
Review the removed stable IDs and their last-good names; **Copy removed IDs** helps
locate references. Update affected references, then use **Accept reviewed removal**.
If source data or the active generation has changed since review, the acceptance
fails safely; use **Reimport / review again** and inspect the new result.

The same explicit API action uses `allow_removed_outputs: true`, with
`expected_generation` and `expected_active_generation` taken from the reviewed
job's `result.generation` and `result.previous_generation`. Use the same source and
settings. This accepts removal; it does not automatically remap references or delete
scene components. A normal GLB without persistent custom
IDs uses structural matching, which cannot guarantee identity after rename or
restructuring.

## Coordinates and supported content

The static profile uses right-handed glTF coordinates: metres, Y up. The Blender
helper enables glTF's Y-up conversion; do not add a second manual axis conversion.
Transforms and hierarchy are retained. Physics currently supports explicit boxes
on root-level objects. A visual imported mesh is not a mesh collider.

The renderer uses static triangle meshes, UV0, base-color factor/texture, metallic
and roughness factors, and directional light/shadows. Import can retain additional
glTF material metadata, but normal/occlusion/emissive/metallic-roughness texture maps,
glTF alpha-mode selection, unlit selection and per-material face-culling behavior
are not all implemented in the baseline renderer. Diagnostics identify unsupported
material features. Procedural Blender node networks need baking or simplification.
Skinning, animation playback, morph targets and compressed geometry are outside this
profile. Imported material records use the versioned `faset.material` format inside
the asset manifest; a standalone material editor is later work.

## What to commit and how to rebuild caches

Commit your source files, Blender bundle manifests/payloads, and
`<source>.faset-import.json` sidecars. Sidecars preserve AssetId and import settings.
Also commit `<source>.faset-overrides.json` if used. Commit `.blend` sources when they
are part of your project. Do not commit `.faset/cache`.

After clearing the cache or cloning a project, import the same sources again. Their
sidecars restore identities and recipes. Rebuild C++ to restore gameplay metadata.
Deleting the **entire** `.faset` directory also removes recovery journals and local
editor state; preserve unsaved work before doing that. An export fails if a referenced
asset has not yet been rebuilt.

The cache key includes source/dependency hashes, settings, importer recipe, pinned
importer dependencies and the desktop target profile. Linux and Windows share this
portable content profile; runtime executable builds remain platform-specific.

For an end-to-end example, open `examples/projects/collect-3d`. Its exit arch includes
a `.blend`, reproducible Blender script and a published bundle. The repository's
`tools/verify_blender_roundtrip.py` runs the actual Blender helper and Editor imports
to check rename, geometry changes, removal conflicts and failure preservation.
Pass `--ui-probe build/linux-debug/faset_blender_editor_probe` to also test two
instances in one live editor session: both receive new geometry while preserving
placement, tint, opaque gameplay fields and the authoring revision. This additional
probe requires Vulkan and produces before/after captures.
