"""CPU-only schema-export and Player validation contracts for optional Lua."""

import argparse
import json
from pathlib import Path
import subprocess
import tempfile


parser = argparse.ArgumentParser()
parser.add_argument("--exporter", required=True)
parser.add_argument("--player")
parser.add_argument("--disabled", action="store_true")
args = parser.parse_args()


def run(executable, *arguments, success=True):
    result = subprocess.run([executable, *map(str, arguments)], capture_output=True,
                            text=True, encoding="utf-8", timeout=20)
    assert (result.returncode == 0) == success, (result.stdout, result.stderr)
    return result


def behavior(type_id="test.lua.cli", callback='error("LUA_CALLBACK_MUST_NOT_RUN")'):
    return f'''local behavior = faset.behavior {{
    id = "{type_id}", version = 1, name = "CLI behavior",
    fields = {{ speed = {{ type = "number", default = 3, min = 0 }} }}
}}
function behavior:on_start() {callback} end
return behavior
'''


with tempfile.TemporaryDirectory(prefix="faset-lua-cli-") as temporary:
    root = Path(temporary) / "Lua Café 世界"
    scripts = root / "Scripts"
    scripts.mkdir(parents=True)
    manifest = root / "project.faset.json"
    manifest.write_text(json.dumps({"format": "faset.project", "version": 1}), encoding="utf-8")
    native = json.loads(run(args.exporter).stdout)
    assert json.loads(run(args.exporter, "--project", root).stdout) == native
    manifest.write_text(json.dumps({"format": "faset.project", "version": 1,
                                   "scripting": {"lua": {"scripts": ["Scripts/cli.lua"]}}}),
                        encoding="utf-8")
    source = scripts / "cli.lua"
    source.write_text(behavior(), encoding="utf-8")
    if args.disabled:
        result = run(args.exporter, "--project", root, success=False)
        assert "without Lua support" in result.stderr, result.stderr
        if args.player:
            run(args.player, "--help")
        print("Lua-disabled schema exporter accepts C++ projects and rejects Lua clearly")
    else:
        exported = root / "schema.json"
        run(args.exporter, "--project", root, "--output", exported)
        merged = json.loads(exported.read_text(encoding="utf-8"))
        assert merged["format"] == "faset.schema" and merged["version"] == 1
        by_id = {item["id"]: item for item in merged["types"]}
        assert len(by_id) == len(native["types"]) + 1
        assert by_id["test.lua.cli"]["fields"]["speed"]["default"] == 3
        for item in native["types"]:
            assert by_id[item["id"]] == item

        # Lua lifecycle errors do not execute in the schema exporter or --validate.
        scene = root / "scene.scene.json"
        scene.write_text(json.dumps({
            "format": "faset.scene", "version": 1, "dimension": 2,
            "id": "lua-cli-scene", "name": "Lua CLI", "instances": [],
            "entities": [{"id": "lua-object", "name": "Lua", "parent": None,
                          "components": [{"id": "lua-behavior", "type": "test.lua.cli",
                                          "version": 1, "fields": {"speed": 3}}]}]
        }), encoding="utf-8")
        if args.player:
            for project_arguments in [("--project", root), ()]:
                result = run(args.player, "--scene", scene, "--validate", *project_arguments)
                assert json.loads(result.stdout)["validated"]
                assert "LUA_CALLBACK_MUST_NOT_RUN" not in result.stderr
            valid_scene = scene.read_text(encoding="utf-8")
            invalid_scene = json.loads(valid_scene)
            for invalid_speed in ["fast", -1]:
                invalid_scene["entities"][0]["components"][0]["fields"]["speed"] = invalid_speed
                scene.write_text(json.dumps(invalid_scene), encoding="utf-8")
                result = run(args.player, "--scene", scene, "--validate", "--project", root,
                             success=False)
                assert "speed" in result.stderr, result.stderr
                assert "LUA_CALLBACK_MUST_NOT_RUN" not in result.stderr, result.stderr
            scene.write_text(valid_scene, encoding="utf-8")
            run(args.player, "--scene", scene, "--validate", "--watch-lua", success=False)

        # A failed schema candidate never overwrites a previously published file.
        previous = exported.read_bytes()
        source.write_text("this is invalid Lua!", encoding="utf-8")
        result = run(args.exporter, "--project", root, "--output", exported, success=False)
        assert "cli.lua" in result.stderr, result.stderr
        assert exported.read_bytes() == previous
        source.write_text(behavior("faset.transform"), encoding="utf-8")
        run(args.exporter, "--project", root, success=False)
        if native["types"]:
            source.write_text(behavior(native["types"][0]["id"]), encoding="utf-8")
            run(args.exporter, "--project", root, success=False)
        source.write_text("while true do end", encoding="utf-8")
        run(args.exporter, "--project", root, success=False)
        source.write_text(behavior(), encoding="utf-8")
        assert json.loads(run(args.exporter, "--project", root).stdout) == merged
        print("Lua schema merge, lifecycle-free validation, failure isolation and budgets passed")
