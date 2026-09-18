#!/usr/bin/env python3
"""Run unmodified Blender and the real Editor importer; verify rename and failure recovery."""
import argparse
import json
import pathlib
import shutil
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]


def verify(blender, editor, output, ui_probe=None):
    fixture = output / "fixture"
    result = subprocess.run([str(blender), "--background", "--factory-startup", "--python-exit-code", "1",
        "--python", str(ROOT / "tests/blender/generate_fixture.py"), "--", str(ROOT), str(fixture)],
        check=True, capture_output=True, text=True)
    (output / "blender.log").write_text(result.stdout + result.stderr)
    assert "FASET_BLENDER_ROUNDTRIP_EXPORT_OK" in result.stdout
    project = output / "project"
    source = project / "Assets/door"
    source.mkdir(parents=True, exist_ok=True)
    def command(name, arguments):
        process = subprocess.run([str(editor), "--project", str(project), "--command",
            json.dumps({"name": name, "arguments": arguments}), "--wait"], capture_output=True, text=True)
        assert process.stdout, process.stderr
        return json.loads(process.stdout)
    def import_stage(stage):
        shutil.copytree(fixture / stage, source, dirs_exist_ok=True)
        return command("faset_import", {"path": "Assets/door/manifest.json"})
    first = import_stage("initial")
    assert first["state"] == "succeeded", first
    identity = first["result"]["asset_id"]
    manifest = first["result"]["manifest"]
    # Keep an authoring scene with independently owned placement/material values.
    document = command("faset_document_create", {"name": "Roundtrip", "dimension": 3})
    scene = document["scene"]
    scene["entities"] = [{"id": "door", "name": "Gameplay door", "parent": None, "components": [
        {"id": "door-pose", "type": "faset.transform", "version": 1,
         "fields": {"position": [4, 0, 2], "rotation": [0, 0, 0], "scale": [1, 1, 1]}},
        {"id": "door-mesh", "type": "faset.mesh", "version": 1,
         "fields": {"asset": identity, "primitive": "asset", "color": [0.5, 0.8, 0.3, 1]}}]}]
    scene_file = project / "roundtrip.scene.json"
    scene_file.write_text(json.dumps(scene))
    before = scene_file.read_bytes()
    renamed = import_stage("renamed")
    assert renamed["state"] == "succeeded", renamed
    assert renamed["result"]["asset_id"] == identity
    assert renamed["result"]["generation"] != first["result"]["generation"]
    def ids(value):
        return sorted(value["outputs"])
    assert ids(manifest) == ids(renamed["result"]["manifest"]), (manifest, renamed)
    active = command("faset_assets", {})
    removed = import_stage("removed")
    assert removed["state"] == "conflict", removed
    assert command("faset_assets", {}) == active
    (source / "manifest.json").write_text('{"broken":true}')
    failed = command("faset_import", {"path": "Assets/door/manifest.json"})
    assert failed["state"] == "failed", failed
    assert command("faset_assets", {}) == active
    assert scene_file.read_bytes() == before
    report = {"blender": subprocess.check_output([str(blender), "--version"], text=True).splitlines()[0],
              "asset_id": identity, "initial_generation": first["result"]["generation"],
              "renamed_generation": renamed["result"]["generation"],
              "stable_output_ids": True, "removed_output_conflict": True,
              "failed_import_keeps_generation": True, "authoring_preserved": True}
    if ui_probe:
        probe = subprocess.run([str(ui_probe), str(fixture), str(output / "live-editor")],
                               capture_output=True, text=True, encoding="utf-8")
        (output / "live-editor.log").write_text(probe.stdout + probe.stderr, encoding="utf-8")
        probe.check_returncode()
        report["live_editor"] = json.loads((output / "live-editor/probe.json").read_text())
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--blender", type=pathlib.Path, required=True)
    parser.add_argument("--editor", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--ui-probe", type=pathlib.Path,
                        help="faset_blender_editor_probe: verify two live viewport instances (Vulkan required)")
    args = parser.parse_args()
    if args.output:
        args.output.resolve().mkdir(parents=True, exist_ok=True)
        verify(args.blender.resolve(), args.editor.resolve(), args.output.resolve(),
               args.ui_probe.resolve() if args.ui_probe else None)
    else:
        with tempfile.TemporaryDirectory(prefix="faset-blender-") as temporary:
            verify(args.blender.resolve(), args.editor.resolve(), pathlib.Path(temporary),
                   args.ui_probe.resolve() if args.ui_probe else None)
