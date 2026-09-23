# Use the Editor through MCP

Faset exposes the same authoring commands to its native interface and to MCP. MCP
runs in the **Editor**. It never provides access to a running game's entities, and
is not linked into the Player or SchemaExporter.

Start a headless server after building Faset:

```sh
build/linux-debug/faset_editor --project /absolute/path/to/game --mcp
```

Configure your MCP client to launch that executable with those arguments using a
stdio transport. On Windows select `build/windows-debug/faset_editor.exe`. Use
absolute paths, including the project path. Add `--gui` when the same process
should display the native Editor; a graphical session and supported GPU are then
required. A separately launched Editor process owns a separate in-memory session.

The transport uses newline-delimited JSON-RPC and the MCP `2025-06-18` lifecycle.
Initialize, send `notifications/initialized`, then discover commands with
`tools/list`. Standard output is reserved for protocol messages. See the official
[MCP transport](https://modelcontextprotocol.io/specification/2025-06-18/basic/transports)
and [tool contracts](https://modelcontextprotocol.io/specification/2025-06-18/server/tools).

## Authoring workflow

1. Use `faset_documents` or create/open a document.
2. Query `faset_schema` for stable type and field IDs and their constraints.
3. Query the document for its persistent ID and current revision.
4. Submit one `faset_scene_edit` batch with that revision.
5. Save with `faset_document_save`. Use Undo/Redo for authoring changes.

For example, the arguments to `faset_scene_edit` can be:

```json
{
  "document": "REPLACE_WITH_DOCUMENT_ID",
  "revision": 0,
  "idempotency_key": "create-first-object",
  "operations": [{"op": "entity.create", "name": "Player"}]
}
```

The command returns the new revision and scene with generated IDs. Repeating the
same batch and retry key in the same session returns its previous result. Reusing
the key for a different payload is an error. An outdated revision produces
`revision.conflict`; query the new state and decide how to apply the intended change.
Do not blindly retry a write using a fresh revision.

Each successful batch is one Undo step. A failed operation rejects the whole batch.
Manual Inspector edits use this same service, including revision checks.

## Jobs and capabilities

`faset_import`, `faset_build`, `faset_export`, and `faset_play` return job IDs.
Use `faset_job` or `faset_jobs` for progress, diagnostics and results.
`faset_job_cancel` requests cancellation; cancelling a build is separate from
undoing a document change. Failed compilation/import preserves the last successful
published generation.

`faset_capabilities` reports available services. `faset_editor_capture` is present
only in a graphical Editor. It returns an MCP image and can save a project-relative
PNG. A headless server has no screenshot tool. `faset_play_control` provides pause,
resume and single-step process controls; it cannot read or modify game entities.

`faset_recovery_list` and `faset_recovery_restore` expose crash recovery, including
scenes that have never been saved. Restoring an already open document requires its
current revision. Recovery refuses to overwrite an externally changed scene file.

Long-lived headless and graphical Editor sessions also poll the same autosave
controller. With `editor.autosave` enabled (the default), a named dirty scene is
saved after two seconds idle. `faset_autosave_status {}` reports `enabled` and each
open document's `state`, `revision`, `path`, and `error`. States include `saved`,
`pending`, `saving`, `conflict`, `failed`, `save_as_required`, and `disabled`.
An unnamed scene remains in recovery until you give it an explicit path. An
external disk edit produces `conflict` without overwriting that edit. For scripted
workflows, use `faset_document_save` with `expected_revision` when you need a
definite save boundary instead of waiting for idle autosave. If a conflict occurs,
save to a new project-relative path and compare the two files. Change the
preference through `faset_project_settings_get` and `faset_project_settings_set`
with the returned project revision and `{"editor":{"autosave":false}}`; the
new value applies to the current session and persists for the next Editor launch.

## Single-command CLI

The CLI is useful for scripts that do not need an MCP session:

```sh
build/linux-debug/faset_editor --project /absolute/path/to/game \
  --command '{"name":"faset_import","arguments":{"path":"Assets/door/manifest.json"}}' --wait
```

`--wait` follows a returned job until completion. Failed, cancelled and conflicting
jobs produce a nonzero exit code. This shell quoting example targets Bash; use the
appropriate argument quoting for your Windows shell.

Validation: unit tests cover protocol errors and shared authoring semantics.
`editor_mcp_stdio` launches the real Editor and verifies initialization, clean JSON
stdout, conflicts, retry behavior, Undo/Redo, saving, reopening, and orderly EOF.
