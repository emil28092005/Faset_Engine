# P1 Gameplay Iteration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close PLAN P1 with reliable build/schema reuse, useful diagnostics and source navigation, four runnable project starters, safe scene autosave, repeatable iteration evidence, and Linux/Windows Lua validation.

**Architecture:** Keep CMake/Ninja responsible for native dependency analysis and retain immutable last-good build generations. Add a content-checked schema/package cache after native build, normalize diagnostics into the existing job API, run revision-aware autosave in `Session::poll`, and expose all new state through Editor commands shared by GUI and MCP.

**Tech Stack:** C++20, CMake/Ninja, Clang/clang-cl, optional Lua 5.4, SDL3/Vulkan 1.3 for graphical tests, Python 3 for workflow measurements, MkDocs Material.

**Spec:** `docs/superpowers/specs/2026-09-24-p1-iteration-design.md`

## Global Constraints

- Linux and Windows x86-64 desktop 2D/3D are the supported P1 targets; all user-facing Editor copy and Manual pages are English.
- C++ is compiled into the Player; Lua is optional and development reload resets state. No C++ hot reload or dynamic gameplay loading is required.
- MCP addresses Editor authoring/build services, never the live Player world.
- Build and export failures preserve the last successful generation and schema; Debug and Release native trees stay separate.
- CMake/Ninja always run for a requested native build; cache reuse starts only after their success and artifact verification.
- The existing recovery journal protects every authoring transaction; autosave never overwrites a disk conflict or gives an unnamed scene an implicit path.
- The two `docs/design/p1-*-reference.png` images guide visual quality only; `PLAN.md` and the spec define behavior. Paths are cross-platform.
- Keep `BuildService::scaffold(name, dimension)` and `faset_script_open` working for existing callers.
- Before every RED CTest run, register a new named test if needed, reconfigure and rebuild its executable; `ctest --no-tests=error` prevents an empty match from passing.

## Review Focus

- An included project header changes while a build runs: reject the mixed candidate and keep the last-good pointer; Tasks 1–2 pin this.
- A toolchain executable changes in place while its path remains the same: invalidate the native build tree/package key; Task 1 pins this.
- A valid cached manifest points to a truncated schema or shader: do not return a hit; Task 2 pins this.
- An external editor or another MCP client changes a scene before autosave: never overwrite it or a newer revision; Task 6 pins this.
- A Windows diagnostic contains a drive colon, Unicode directories and a line/column: parse the right location and reject navigation outside `Scripts`; Tasks 3–4 pin this.

---

### Task 1: Capture complete gameplay inputs and toolchain identity

**Files:** Create `include/faset/editor/build_cache.hpp`, `src/editor/build_cache.cpp`, `tests/build_cache_tests.cpp`; modify `cmake/BuildService.cmake`, `src/editor/build_service.cpp`, `src/editor/session.cpp`.

**Interfaces:** `BuildInputs capture_build_inputs(const BuildConfig&, const scripting::LuaProject&)` records `source_hash` over all regular files under project `Scripts` plus Lua declaration/fingerprint, and separate recipe/toolchain hashes over normalized configure args and executables. `BuildInputs::fingerprint()` combines these fields deterministically. `ensure_native_toolchain_stamp(native_directory, inputs)` forces a fresh native tree if the compiler/CMake/Slang identity changed in place. Extract the common Scripts snapshot so `Session::source_signature()` and `BuildService` use the same content set, while allowing their intended extra fields to differ.

- [ ] **Step 1: Write failing source/identity tests.** In `tests/build_cache_tests.cpp`, construct a temporary project with `Gameplay.cpp`, `Gameplay.hpp` and `Scripts/Extensions/Extra.hpp`; change only the nested header, Lua declaration and then bytes of a fake compiler at the same path. Each change must alter the corresponding fingerprint. A symlink escaping `Scripts` must be rejected. Test the toolchain stamp with a disposable build directory. Register `faset_build_cache_tests` and CTest name `build_cache` in `cmake/BuildService.cmake` before the red run.
  ```cpp
  const auto before = capture_build_inputs(config, lua);
  atomic_write(root / "Scripts/Extensions/Extra.hpp", "#define SPEED 2\n");
  require(capture_build_inputs(config, lua).fingerprint() != before.fingerprint(),
          "Nested gameplay header invalidates the source snapshot");
  ```
- [ ] **Step 2: Build the red test.** Run `cmake --preset linux-debug`, then `cmake --build --preset linux-debug --target faset_build_cache_tests --parallel 4`. The test target is registered; compilation must fail specifically on the missing `capture_build_inputs` interface, not on an unknown target.
- [ ] **Step 3: Implement the input scanner and toolchain stamp.** Sort project-relative UTF-8 paths, hash file bytes, reject escaping symlinks, include explicit recipe/version values, and hash resolved tool executables. Do not use modification time alone. Compare/persist the stamp before native configure; on a changed stamp clear only the generated native tree for that configuration, never published generations or source files. Replace the narrower two-file post-build race check with the captured complete Scripts snapshot.
  ```cpp
  const auto submitted = capture_build_inputs(config, job.lua);
  ensure_native_toolchain_stamp(native_directory, submitted);
  // After native build and schema extraction, before publication:
  if (capture_build_inputs(config, scripting::loadLuaProject(config.project_root))
          .source_hash != submitted.source_hash)
      throw std::runtime_error("Gameplay sources changed during the build; build again");
  ```
- [ ] **Step 4: Rebuild and run focused tests.** Build `faset_build_cache_tests faset_build_schema_tests faset_editor_session_tests`, then run `ctest --test-dir build/linux-debug --no-tests=error --output-on-failure -R '^(build_cache|build_schema_publication|editor_session_settings)$'`. All three named tests pass; a changed nested header during the fixture build rejects publication.
- [ ] **Step 5: Commit** `Capture complete gameplay and toolchain build inputs`.

### Task 2: Reuse only verified schema/package generations

**Files:** Modify `include/faset/editor/build_service.hpp`, `src/editor/build_cache.cpp`, `src/editor/build_service.cpp`, `tests/build_cache_tests.cpp`, `tests/build_schema_tests.cpp`, `tests/build_service_tests.cpp`.

**Interfaces:** `build_package_key(inputs, native_directory, configuration)` hashes the post-build Player, SchemaExporter, required SPIR-V/reflection files, runtime libraries and CMake cache. `validate_build_generation(directory, key)` verifies every recorded file hash and validates the schema. Successful `BuildService::build()` returns `schema_cache_hit`, `generation_reused`, `fingerprint`, and phase times without changing existing result paths.

- [ ] **Step 1: Write failing cache and invalidation tests.** The fixture `tests/build_schema_tests.cpp` must count SchemaExporter invocations: two unchanged builds return the same generation and count one export; a changed C++ header, Lua source, configure option, shader bytes or runtime artifact produces a new generation; corrupt or missing cached schema/shader is never a hit. Extend `tests/build_service_tests.cpp` so a failed compile preserves `last_build.json`, and export after changed asset source fails until reimport, then packages the new asset generation despite a gameplay cache hit.
  ```cpp
  const auto first = builds.wait(builds.start_build());
  const auto again = builds.wait(builds.start_build());
  check(again.result.at("generation") == first.result.at("generation") &&
            again.result.at("schema_cache_hit") == true &&
            again.result.at("generation_reused") == true,
        "Unchanged build reuses a verified generation");
  ```
- [ ] **Step 2: Run rebuilt red tests.** Build `faset_build_cache_tests faset_build_schema_tests faset_build_service_tests`, then run `ctest --test-dir build/linux-debug --no-tests=error --output-on-failure -R '^(build_cache|build_schema_publication|process_and_cook)$'`. At least the newly added cache assertion must fail on missing flags or duplicate schema export; an old binary passing does not count.
- [ ] **Step 3: Implement post-native cache lookup and manifest hashes.** Continue to run configure/build first. Compute the package key, verify the previous generation's manifest/files/schema, then return it without SchemaExporter/copy if valid. Otherwise export schema into staging from the captured Lua snapshot, copy files, write per-file hashes and key, recheck source snapshot, and atomically update `last_build.json`. Never return a damaged generation. Keep the raw job log and phase timing for a hit as well as a miss.
  ```cpp
  if (auto previous = verified_generation(config.cache_root, key))
      return result_for(*previous, /*schema_cache_hit=*/true,
                        /*generation_reused=*/true);
  // Only a fully validated staging directory may become last_build.json.
  ```
- [ ] **Step 4: Rebuild and run focused/integration tests.** Build `faset_build_cache_tests faset_build_schema_tests faset_build_service_tests faset_editor_session_tests`, then run `ctest --test-dir build/linux-debug --no-tests=error --output-on-failure -R '^(build_cache|build_schema_publication|process_and_cook|editor_session_settings)$'`. All four pass. Inspect one real no-op build's log: Ninja does no native compile/link; SchemaExporter is absent on the second request. Do not call this a native cache hit unless the log confirms it.
- [ ] **Step 5: Commit** `Reuse verified gameplay schema and build generations`.

### Task 3: Parse structured compiler and Lua diagnostics

**Files:** Create `include/faset/editor/build_diagnostics.hpp`, `src/editor/build_diagnostics.cpp`, `tests/build_diagnostics_tests.cpp`; modify `cmake/BuildService.cmake`, `include/faset/editor/build_service.hpp`, `src/editor/build_service.cpp`.

**Interfaces:** `parse_build_diagnostics(raw_log, phase, project_root)` returns a bounded JSON array of `{severity, phase, message, file?, line?, column?, code?}`. `JobStatus::diagnostics` is serialized by `JobStatus::json()` and returned by `faset_job`; `log` and `error` remain unchanged for old clients.

- [ ] **Step 1: Write failing parser fixtures.** Cover `/project/Scripts/Game.cpp:17:4: error:`, `C:\\Café\\Scripts\\Game.cpp(17,4): error`, `C:\\Café\\Scripts\\Game.cpp:17:4: warning:`, `Scripts/player.lua:6: unexpected symbol`, ANSI escapes, multiline note/caret output, an engine/external path, and a nonzero process with only unparseable text. Assert one-based positive locations and normalized project-relative files only for files under `Scripts`. Register `faset_build_diagnostics_tests` and CTest name `build_diagnostics` in `cmake/BuildService.cmake`.
  ```cpp
  const auto rows = parse_build_diagnostics(
      "Scripts/Game.cpp:17:4: error: bad field\n", "compile", project);
  require(rows.size() == 1 && rows[0].at("file") == "Scripts/Game.cpp" &&
              rows[0].at("line") == 17 && rows[0].at("severity") == "error",
          "Clang location is structured");
  ```
- [ ] **Step 2: Build the red parser test.** Run `cmake --preset linux-debug`, then `cmake --build --preset linux-debug --target faset_build_diagnostics_tests --parallel 4`. Compilation must fail on the missing parser API, not an unknown test target.
- [ ] **Step 3: Implement incremental job collection.** Parse completed log lines as process output arrives, cap row count/message size, strip ANSI, and preserve every raw line in the bounded existing log. Use phase names from `BuildService` checkpoints. On a failed process with no parsed error, add a generic diagnostic with the exit code and `see job log`; never fabricate a navigable file.
  ```cpp
  job.status.diagnostics = parse_build_diagnostics(job.status.log, job.status.stage,
                                                    config.project_root);
  if (exit_code != 0 && job.status.diagnostics.empty())
      job.status.diagnostics.push_back(generic_process_error(exit_code));
  ```
- [ ] **Step 4: Run parser and real failure tests.** Build `faset_build_diagnostics_tests faset_build_service_tests faset_build_schema_tests`, then run `ctest --test-dir build/linux-debug --no-tests=error --output-on-failure -R '^(build_diagnostics|process_and_cook|build_schema_publication)$'`. All three pass; a deliberate `#error` in `Gameplay.cpp` yields both a structured row and the original log, with previous build retained.
- [ ] **Step 5: Commit** `Expose structured gameplay build diagnostics`.

### Task 4: Open project source at a diagnostic location

**Files:** Modify `src/editor/session.cpp`, `src/editor/editor_ui.cpp`, `tests/editor_session_tests.cpp`, `tests/editor_ui_tests.cpp`, `docs/manual/editor/diagnostics.md`.

**Interfaces:** New command `faset_source_open({path, line?, column?, editor?})` validates project code source and opens it with an argv template. Existing `faset_script_open` delegates to the same safe launcher while retaining Lua-only validation. `editor.script_editor` accepts `{file}`, `{line}`, `{column}` and `{project}` tokens. The Console uses Task 3 `diagnostics` and displays cache/timing fields from Task 2.

- [ ] **Step 1: Write failing command/UI tests.** A valid `.cpp` and `.lua` under `Scripts` produce the configured argv with line/column; use a disposable probe executable to record arguments without launching a real editor. A drive/Unicode project path stays one argv element; `../`, symlink escape, directory, `.exe` and nonexistent targets fail with structured `source.*` errors. Clicking a parsed diagnostic issues `faset_source_open` with its location; an external diagnostic has no enabled Open action. Existing `faset_script_open` rejects `.cpp` as before.
  ```cpp
  const auto opened = commands.call("faset_source_open",
      {{"path", "Scripts/Gameplay.cpp"}, {"line", 17}, {"column", 4},
       {"editor", {path_to_utf8(probe_executable), "{file}:{line}:{column}"}}});
  require(opened.at("line") == 17 && opened.at("path") == "Scripts/Gameplay.cpp",
          "Project source opens at the diagnostic position");
  ```
- [ ] **Step 2: Run rebuilt red command/UI tests.** Build `faset_editor_session_tests faset_editor_ui_tests`, then run `ctest --test-dir build/linux-debug --no-tests=error --output-on-failure -R '^(editor_session_settings|editor_ui_authoring)$'` with the configured Vulkan ICD. At least the new source-open assertion must fail; a missing GPU or old binary is not the intended red result.
- [ ] **Step 3: Implement the safe source launcher and Console action.** Use `project_path` and extension/regular-file checks, exact argv substitution with no shell, and Zed's verified `path:line:column` default. Keep raw output expandable, show severity/file/line first, and expose a clear editor-launch error without changing build state. Make rows keyboard reachable and update the English diagnostics Manual.
  ```cpp
  commands_.add("faset_source_open", description, source_schema,
                [&](const Json& args) { return open_project_source(args, /*lua_only=*/false); });
  // UI: call("faset_source_open", {{"path", row.at("file")},
  //                                {"line", row.value("line", 1)}});
  ```
- [ ] **Step 4: Run focused tests with software Vulkan for UI.** Rebuild `faset_editor_session_tests faset_editor_ui_tests`, then run `ctest --test-dir build/linux-debug --no-tests=error --output-on-failure -R '^(editor_session_settings|editor_ui_authoring)$'` on an equipped GPU/software ICD. Both pass; `faset_job` JSON still exposes raw log and normalized rows headlessly.
- [ ] **Step 5: Commit** `Navigate from build diagnostics to project source`.

### Task 5: Four runnable C++/Lua new-project choices

**Files:** Modify `include/faset/editor/build_service.hpp`, `src/editor/build_service.cpp`, `include/faset/editor/project_launcher.hpp`, `src/editor/project_launcher.cpp`, `apps/editor_main.cpp`, `tests/build_service_tests.cpp`, `tests/editor_ui_launcher.cpp`; create `tools/project_templates/lua-main.lua`; update `docs/manual/getting-started/build.md`.

**Interfaces:** New overload `BuildService::scaffold(name, dimension, language)` accepts `cpp|lua` and creates a runnable template; the legacy two-argument call retains its existing C++ scaffold behavior without implicitly creating a scene. `ProjectSelection::language` and `faset_editor --new NAME --dimension 2|3 --language cpp|lua` select the same templates. Explicit template creation adds a minimal `Scenes/main.scene.json`; Lua-only templates receive a declared `Scripts/main.lua` without `Gameplay.cpp/.hpp`.

- [ ] **Step 1: Write failing scaffold/launcher tests.** Create all four explicit combinations under disposable Unicode project paths. Assert dimension/language, scene validity, declared Lua schemas, no C++ stub in Lua projects, ability to `--validate` or launch a built Player, and that existing files are never overwritten. Test legacy `scaffold(name, dimension)` still creates C++ without unexpectedly creating a scene.
  ```cpp
  builds.scaffold("Lua 2D", 2, "lua");
  require(fs::exists(root / "Scripts/main.lua") &&
              !fs::exists(root / "Scripts/Gameplay.cpp"),
          "Lua starter is genuinely Lua-only");
  ```
- [ ] **Step 2: Run rebuilt red tests.** Build `faset_build_service_tests faset_project_launcher_tests`, then run `ctest --test-dir build/linux-debug --no-tests=error --output-on-failure -R '^(process_and_cook|editor_ui_launcher)$'` with the configured Vulkan ICD. Compilation or a new assertion must fail specifically on language selection; old binaries or a missing GPU do not establish red.
- [ ] **Step 3: Add starter files and cross-platform chooser.** Generate the start scene from authoring schema helpers; use source templates for behavior text and LuaLS defaults; reject an existing conflicting destination before writing any starter file. Maintain the dark Editor style, keyboard selection and platform-native path display. The PNG reference guides layout only. Extend CLI help and Manual with exact commands.
  ```cpp
  void BuildService::scaffold(const std::string& name, int dimension,
                              std::string_view language);
  // Existing two-argument overload keeps the legacy scaffold without a scene.
  ```
- [ ] **Step 4: Run focused and real template builds.** Rebuild `faset_build_service_tests faset_project_launcher_tests faset_player`, then run `ctest --test-dir build/linux-debug --no-tests=error --output-on-failure -R '^(process_and_cook|editor_ui_launcher|lua_cli_contracts)$'` with the configured Vulkan ICD. All three pass; build/validate one generated C++ and one generated Lua project for each dimension.
- [ ] **Step 5: Commit** `Offer runnable C++ and Lua project starters`.

### Task 6: Revision-aware autosave controller and MCP status

**Files:** Create `include/faset/editor/autosave.hpp`, `src/editor/autosave.cpp`, `tests/editor_autosave_tests.cpp`; modify `CMakeLists.txt`, `include/faset/authoring/service.hpp`, `src/authoring/service.cpp`, `src/editor/commands.cpp`, `include/faset/editor/session.hpp`, `src/editor/session.cpp`.

**Interfaces:** `AuthoringService::save(document, path={}, expected_revision=std::nullopt)` rejects a stale expected revision. `AutosaveController` takes `SaveFn = std::function<Json(const std::string&, std::uint64_t)>` in its constructor; `observe(documents, now, enabled)` tracks each document's revision/idle deadline and invokes that callback after 2 seconds. `Session::poll()` calls it in both GUI and long-lived MCP modes. Read-only `faset_autosave_status` returns per-document `state`, `revision`, `path`, `error` and enabled flag.

- [ ] **Step 1: Write failing fake-clock and authoring tests.** Coalesce three edits within 2 seconds into one save; unnamed dirty scene stays in recovery; disabling autosave leaves only recovery; an external disk edit fails with `save.disk_conflict`; a newer revision fails with `revision.conflict` then gets a new deadline; a permission/write error remains visible without frame-by-frame retries; Undo after save still restores the prior scene; a Play snapshot captured before autosave remains byte-identical. Register `faset_editor_autosave_tests` and CTest name `editor_autosave` in `CMakeLists.txt`.
  ```cpp
  service.transact(id, revision, rename_ops);
  controller.observe(service.documents(), start + 1900ms, true);
  require(read_json(scene_path).at("name") == "Before", "Idle timer has not fired");
  controller.observe(service.documents(), start + 2100ms, true);
  require(read_json(scene_path).at("name") == "After", "Named scene autosaved once");
  ```
- [ ] **Step 2: Build the red autosave test.** Run `cmake --preset linux-debug`, then `cmake --build --preset linux-debug --target faset_editor_autosave_tests --parallel 4`. Compilation must fail on the missing controller/revision-aware save API, not an unknown target.
- [ ] **Step 3: Implement deterministic scheduling and status.** Use monotonic time injected into the controller, reset deadline only when revision changes, save with expected revision, suppress identical failure repeats until another edit or explicit retry, and never assign a path to an unnamed scene. Keep recovery journaling and disk-hash guard unchanged. Add `expected_revision` as an optional JSON field to `faset_document_save`; preserve existing callers.
  ```cpp
  if (summary.at("dirty") && !summary.at("path").get<std::string>().empty() &&
      now >= state.deadline)
      authoring.save(id, {}, state.observed_revision);
  ```
- [ ] **Step 4: Run authoring/session/MCP tests.** Rebuild `faset_authoring_tests faset_editor_autosave_tests faset_editor_session_tests faset_mcp_tests faset_editor`, then run `ctest --test-dir build/linux-debug --no-tests=error --output-on-failure -R '^(authoring|editor_autosave|editor_session_settings|editor_mcp|editor_mcp_stdio)$'`. All five pass; a headless MCP session that stays open autosaves a named scene after idle.
- [ ] **Step 5: Commit** `Autosave named scenes with revision and disk conflict safety`.

### Task 7: Autosave settings and visible Editor state

**Files:** Modify `src/editor/session.cpp`, `src/editor/editor_ui.cpp`, `tests/editor_session_tests.cpp`, `tests/editor_ui_project_settings.cpp`, `tests/editor_ui_tests.cpp`, `docs/manual/editor/workspace.md`, `docs/manual/editor/mcp.md`.

**Interfaces:** `project.faset.json` may contain `editor.autosave: bool`, defaulting to `true`; `faset_project_settings_set` updates that property with its existing project revision check, applies it immediately to the current session and preserves `editor.script_editor`. Project settings provide an Autosave toggle. Status bar maps `faset_autosave_status` to Saved, Pending autosave, Saving, Save conflict, Save failed and Save As required. `Session` caches the setting and updates it on the command rather than rereading the project file every polling frame.

- [ ] **Step 1: Write failing settings/UI tests.** Missing setting reads as enabled; toggling off/on survives project reopen and does not change `editor.script_editor`; stale settings revision is rejected; UI shows pending then saved after a deterministic tick, conflict persists with Save As/reload guidance, unnamed dirty scene shows Save As required. Keyboard focus reaches the toggle and conflict action.
  ```cpp
  auto settings = commands.call("faset_project_settings_get", Json::object());
  auto changed = commands.call("faset_project_settings_set",
      {{"revision", settings.at("revision")},
       {"settings", {{"editor", {{"autosave", false}}}}}});
  require(changed.at("settings").at("editor").at("autosave") == false,
          "Project autosave preference persists");
  ```
- [ ] **Step 2: Run rebuilt red tests.** Build `faset_editor_session_tests faset_editor_project_settings_ui_tests faset_editor_ui_tests`, then run `ctest --test-dir build/linux-debug --no-tests=error --output-on-failure -R '^(editor_session_settings|editor_ui_project_settings|editor_ui_authoring)$'` with the configured Vulkan ICD. At least the new settings/status assertion must fail for the expected behavior.
- [ ] **Step 3: Implement settings validation and UI.** Merge only the `editor.autosave` field into existing `editor` settings; reject non-Boolean values. Poll the read-only status instead of inferring save from dirty flags. Keep the dark compact reference style while making actual status and actions correct; document autosave vs recovery and MCP explicit save.
  ```cpp
  require(changes["editor"]["autosave"].is_boolean(), "project.autosave",
          "Autosave must be enabled or disabled");
  value["editor"]["autosave"] = changes["editor"]["autosave"];
  ```
- [ ] **Step 4: Run UI and Manual checks.** Rebuild `faset_editor_session_tests faset_editor_project_settings_ui_tests faset_editor_ui_tests`, then run `ctest --test-dir build/linux-debug --no-tests=error --output-on-failure -R '^(editor_session_settings|editor_ui_project_settings|editor_ui_authoring)$'` with the configured Vulkan ICD; all three pass. `python3 -m mkdocs build --strict` from the documentation virtual environment passes. Capture one Editor screenshot to inspect legibility; the image reference is not a pixel-perfect target.
- [ ] **Step 5: Commit** `Show and configure safe Editor autosave`.

### Task 8: Windows and Release Lua execution evidence

**Files:** Create `tools/verify_lua_release_export.py`, `tests/verify_lua_release_export_test.py`; modify `.github/workflows/ci.yml`, `.github/workflows/windows-graphics.yml`; update `docs/validation/lua-module.md` after results exist. `lua_player_reload` is already registered in `cmake/Player.cmake`.

**Interfaces:** The verifier copies `examples/lua` to a disposable Unicode project path, exports a Release Lua-only package with the real Editor, relocates the generation, hides the source project, runs `--validate` and 120 headless rendered frames, and writes a JSON report containing exact revision/build profile/VM presence/source hashes/device/driver/capture hash. It never modifies the checked-in sample.

- [ ] **Step 1: Write failing verifier contract tests.** In `tests/verify_lua_release_export_test.py`, import the verifier's package/report validator and test that it rejects a package containing project `Gameplay.cpp`, a missing declared Lua source, a package that only works with the source project present, or fewer than 120 rendered frames. Its report must distinguish physical GPU and software Vulkan.
  ```python
  assert report["configuration"] == "Release"
  assert report["lua_enabled"] is True
  assert report["relocated"] is True
  assert report["rendered_frames"] == 120
  ```
- [ ] **Step 2: Run and implement the verifier.** Run `python3 -m unittest discover -s tests -p 'verify_lua_release_export_test.py' -v`; confirm it discovers at least four tests and fails on the absent verifier API. Implement the disposable-project export/relocation/validation/frame runner and report validator in `tools/verify_lua_release_export.py`, then rerun the same test until it passes. Run the verifier on Linux with the real Release Editor and record its exact output. A deliberately incomplete disposable package must still be rejected.
  ```sh
  python3 tools/verify_lua_release_export.py --editor build/linux-release/faset_editor --output .cache/p1-lua-release-check
  ```
- [ ] **Step 3: Run actual platform jobs.** Linux native CI runs existing Lua CPU tests; Windows native CI runs them without a GPU. Windows graphics CI runs `lua_player_reload` under its pinned SwiftShader ICD and the Release export verifier. Run the same verifier on Linux with a real GPU when available; exact commands and environment go in the report. A failed CI job is not described as covered. Keep `FASET_ENABLE_LUA=OFF` for C++-only projects, preserve Lua source snapshot hashing, and package required notices; any observed integration defect gets a failing regression before its fix.
  ```sh
  ctest --test-dir build/linux-release --no-tests=error --output-on-failure -R '^(lua_contracts|lua_safety_contracts|lua_cli_contracts|lua_player_reload|build_schema_publication)$'
  ```
- [ ] **Step 4: Store exact evidence and update the Lua record.** Include CI run links, `ctest` totals, report JSON, renderer/driver details and honest unsupported-hardware notes. A functional Windows SwiftShader result is not a physical-Windows-GPU claim.
- [ ] **Step 5: Commit** `Validate Lua gameplay in Windows and Release exports`.

### Task 9: Measure iteration and close P1 documentation

**Files:** Modify `tools/measure_workflows.py`, `docs/manual/editor/export.md`, `docs/manual/editor/profiling.md`, `docs/manual/scripting/lua.md`, `docs/manual/scripting/first-behavior.md`, `docs/ARCHITECTURE.md`, `docs/IMPLEMENTATION.md`, `PLAN.md`; create `tests/measure_workflows_test.py`, `docs/validation/p1-iteration-2026-09-24/README.md` and raw evidence files.

**Interfaces:** Workflow report version 2 retains raw samples with labels and adds median/nearest-rank p95, exact host/toolchain/driver/revision/source-hash metadata, separate cold and warm paths, changed C++ and Lua-to-reload time, Play-to-first-rendered-frame, synthetic Editor input-to-visible-state latency, and a larger generated scene. Timing values are observations, not CI pass/fail thresholds.

- [ ] **Step 1: Write failing report/math tests.** With sample durations `[1, 2, 3, 4, 5]`, median is 3 and nearest-rank p95 is 5; the report rejects an absent revision, mixed Debug/Release samples under one label, missing source hashes, and a sample labeled windowed first frame when the Player profile says `presentation_mode=offscreen`. Fixture generation must be deterministic from a seed.
  ```python
  assert summarize([1, 2, 3, 4, 5]) == {"median": 3, "p95": 5}
  assert len(report["samples"]["warm_unchanged_build"]) >= 5
  ```
- [ ] **Step 2: Run the red Python test.** `python3 -m unittest discover -s tests -p 'measure_workflows_test.py' -v` discovers at least four tests and fails on the missing summary/report behavior before implementation; a zero-test run does not count.
- [ ] **Step 3: Extend the disposable-project workflow and Manual.** Keep raw stdout/stderr and per-run JSON, warm up then repeat each warm/changed case at least five times, record toolchain/GPU/driver and validation/readback settings, and compare reference budgets only for the exact checked-in sample scenes. Measure a 3,000-frame resource-lifecycle run and report explicit memory growth. Document C++/Lua iteration, cache flags, source navigation, templates, autosave/conflict recovery and profile interpretation in English.
  ```python
  def summarize(values):
      ordered = sorted(values)
      return {"median": statistics.median(ordered),
              "p95": ordered[math.ceil(0.95 * len(ordered)) - 1]}
  ```
- [ ] **Step 4: Run full validation and record dated evidence.** `ctest --preset linux-debug --no-tests=error --output-on-failure`, `ctest --test-dir build/linux-release --no-tests=error --output-on-failure`, available Linux GPU tests, Windows native/SwiftShader CI, `python3 -m mkdocs build --strict` from the documentation virtual environment and the workflow script pass or have exact documented failures. Update `PLAN.md` to P1 complete only after every acceptance-matrix row in the spec has evidence; physical Windows GPU remains marked unverified if unavailable. Run `graphify update .` after code changes where `graphify-out/graph.json` exists, then request independent review and fix load-bearing findings.
- [ ] **Step 5: Commit** `Measure and document P1 gameplay iteration`; publish only after required checks and review are complete.
