# Build, Play, and export

Faset compiles C++ gameplay into a separate native Player. Changing C++ requires a
build and restart. Changes to an authoring scene can be tested without saving it:
**Play** captures the current resolved scene, builds gameplay, and opens its own
Player process. **Stop** leaves the authoring document unchanged.

## The iteration loop

1. Edit `Scripts/Gameplay.cpp` and its explicit schema declarations.
2. Stop the running Player.
3. Choose **Build**. CMake/Ninja build native gameplay and the separate
   SchemaExporter refreshes the Inspector schema when inputs changed.
4. Read compiler errors in the Console. Select a project-source diagnostic to open
   its line in your external editor. A failed build retains the previous schema
   and binary; the schema status reports that it is stale.
5. After a successful build, edit the behavior's exposed fields in the Inspector.
6. Choose **Play**. It builds if necessary and launches the captured scene.

Play captures the authoring document when requested. Editing the document while its
build runs does not silently change that snapshot. Runtime movement, spawned objects,
and gameplay progress do not write back into authoring or its Undo history.

Build and Export capture gameplay sources when their worker begins. If you edit C++
while a job waits in the queue, it builds the newer snapshot and the Inspector's
schema status uses that snapshot's hash. An edit during the active build causes
the job to reject changed sources; run Build again after the edit.

The Player supports pause and single-step. Editor controls use a private session
control file; they do not expose runtime entity queries through MCP. Closing the
Player is observed by the Editor, which keeps its logs and authoring state.

An unchanged second build still asks CMake/Ninja to verify their dependency graph.
When the native outputs, gameplay sources, toolchain, shaders and runtime inputs
match the validated package, **Jobs** reports `schema_cache_hit` and
`generation_reused` as true and returns the same verified generation without a
second SchemaExporter run or package copy. A changed `.cpp`, header, Lua declaration,
build recipe or relevant tool invalidates that reuse. A damaged cached package is
not treated as a hit. The job also exposes elapsed phases and a build fingerprint;
see [Developer diagnostics](diagnostics.md) for structured errors and raw logs.

If a build fails, correct the listed source and build again. The prior playable
generation remains available, but stale Inspector metadata is not evidence that
the failed edit built successfully. Save the scene separately: named dirty scenes
autosave after two idle seconds by default, while an unnamed scene needs **Save As**.
External file changes produce a visible conflict instead of an overwrite; the
[workspace guide](workspace.md#create-and-save-a-scene) explains recovery.

## Export a standalone game

Use **Export** in the Editor for the current scene. Resolve template conflicts and
import all referenced assets first. Export validates the scene, builds C++ and
metadata, cooks resources, copies runtime dependencies/notices, and verifies the
package before publishing it.

Through MCP or the command interface, the operation is:

```json
{"name":"faset_export","arguments":{
  "document":"the-open-document-id","output":"Exports/MyGame"
}}
```

The result is a job ID. Poll `faset_job` until `succeeded`, `failed` or `cancelled`.
The successful job's `result` identifies the package. Output paths are relative to
the project; the Editor rejects paths escaping it.

Development builds use **Debug**. Exports default to **Release** and use a separate
CMake cache. The BuildService API also accepts `RelWithDebInfo` for exports. Exporting
does not change the configuration of your development Player.

Each successful export creates a versioned directory under
`Exports/MyGame/generations/<generation>`. `current.json` points to the active
generation. The Editor leaves published generations untouched. Distribute the
**whole generation directory**, not just the executable.
If building, cooking or verification fails, the previous pointer and package remain
available. Cancelling a job does not delete earlier exports.

## Run the package

Open the generation directory and launch `faset_player` on Linux or
`faset_player.exe` on Windows. The package selects its cooked start scene and shader
directory. It does not need the Faset source checkout, Editor, MCP, Blender, CMake,
SchemaExporter or Slang compiler. Vulkan drivers still create native GPU pipelines
from the packaged SPIR-V.

The package includes its resource hashes, build profile and third-party notices.
Keep these files when redistributing it. Faset's own repository license remains an
explicit project-owner decision; third-party notices do not assign an engine license.

## Platform requirements

- **Linux x86-64:** a desktop session and Vulkan loader/driver exposing the baseline
  Vulkan 1.3 features. Build on a distribution compatible with the target machines'
  C/C++ runtime; the export is not an all-distribution static executable.
- **Windows x86-64:** a supported Vulkan driver and the compatible Microsoft Visual
  C++ runtime. Release packages use the dynamic release CRT; install the matching
  x64 Visual C++ Redistributable on the target machine. Debug development builds also
  require development runtime libraries and are not the distribution package.

Build and test Linux packages on Linux, Windows packages on Windows. Cross-compilation
is not part of this MVP. CI uses software Vulkan on Windows for deterministic image
and package execution tests; that is separate from physical GPU-driver testing.

## Troubleshooting

**Unknown component:** enable/register the missing runtime module and rebuild. A
data-only custom component still needs a schema; it does not need callbacks.

**Schema version mismatch:** match the linked gameplay schema to the document or
apply an explicit data migration, then rebuild. The Editor preserves future or
missing component data as opaque fields; export requires a matching schema.

**Missing asset:** import the original source or Blender manifest again. Preserve its
sidecar so the AssetId remains stable. A cache copied from another project is not a
substitute for the correct source identity.

**Shader build error:** fix the Slang diagnostic and build again. Failed compilation
retains the last successful shader generation; it is not reported as a successful
new build. A pipeline reload also checks resource layouts before replacing a working
pipeline.

**No Vulkan device / unsupported feature:** use a compatible driver/device. The
renderer reports the required capability rather than silently selecting a reduced
graphics profile. Headless authoring and schema export do not require a GPU; playing
and capturing images do.
