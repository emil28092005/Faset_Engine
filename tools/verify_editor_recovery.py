#!/usr/bin/env python3
"""Exercise crash recovery, failed Player startup and failed C++ build over real MCP.

Uses a disposable copy of the 2D sample. Requires native build tools and a Vulkan
desktop for the successful Play/Stop check. No runtime entity API is used.
"""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import platform
import queue
import shutil
import subprocess
import threading
import time


class Client:
    def __init__(self, editor, engine, project):
        self.process = subprocess.Popen([str(editor), "--engine", str(engine), "--project",
            str(project), "--mcp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, encoding="utf-8")
        self.output, self.errors, self.sequence = queue.Queue(), [], 0
        def output():
            try:
                for line in self.process.stdout:
                    self.output.put(json.loads(line))
            except Exception as error:
                self.output.put(error)
            finally:
                self.output.put(None)
        def errors():
            for line in self.process.stderr:
                self.errors.append(line)
        self.readers = [threading.Thread(target=output, daemon=True), threading.Thread(target=errors, daemon=True)]
        for reader in self.readers:
            reader.start()
        self.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
            "clientInfo": {"name": "recovery-acceptance", "version": "1"}})
        self.process.stdin.write('{"jsonrpc":"2.0","method":"notifications/initialized"}\n')
        self.process.stdin.flush()

    def request(self, method, params):
        self.sequence += 1
        self.process.stdin.write(json.dumps({"jsonrpc": "2.0", "id": self.sequence,
                                           "method": method, "params": params}) + "\n")
        self.process.stdin.flush()
        response = self.output.get(timeout=30)
        assert isinstance(response, dict) and response.get("id") == self.sequence, response
        assert "error" not in response, response
        return response["result"]

    def call(self, name, arguments=None, error=False):
        result = self.request("tools/call", {"name": name, "arguments": arguments or {}})
        assert result.get("isError", False) == error, result
        return result["structuredContent"]

    def job(self, identity, expected="succeeded"):
        deadline = time.monotonic() + 1800
        while time.monotonic() < deadline:
            value = self.call("faset_job", {"id": identity})
            if value["state"] in ("succeeded", "failed", "cancelled", "conflict"):
                assert value["state"] == expected, value
                return value
            time.sleep(.1)
        raise RuntimeError("Editor job timed out")

    def close(self, crash=False):
        if self.process.poll() is None:
            if crash:
                self.process.kill()
            else:
                self.process.stdin.close()
            try:
                self.process.wait(timeout=30)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
                raise
        for reader in self.readers:
            reader.join(timeout=5)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--editor", type=Path, required=True)
    parser.add_argument("--engine", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    engine, editor, output = args.engine.resolve(), args.editor.resolve(), args.output.resolve()
    if output.exists():
        parser.error("Use a new output directory")
    output.mkdir(parents=True)
    project = output / "project"
    shutil.copytree(engine / "examples/projects/collect-2d", project,
                    ignore=shutil.ignore_patterns(".faset", "Exports"))
    checks = []
    def record(name):
        checks.append(name)
        (output / "progress.json").write_text(json.dumps(checks, indent=2) + "\n")
        print(name, flush=True)
    client = Client(editor, engine, project)
    try:
        doc = client.call("faset_documents")["documents"][0]
        identity = doc["id"]
        client.call("faset_document_save", {"document": identity})
        before = client.call("faset_document_query", {"document": identity})
        changed = client.call("faset_scene_edit", {"document": identity, "revision": before["revision"],
            "operations": [{"op": "scene.rename", "name": "Unsaved recovery acceptance"}]})
        client.close(crash=True)  # Kill only our idle owned Editor, after transaction acknowledgement.
        client = Client(editor, engine, project)
        reopened = client.call("faset_document_query", {"document": identity})
        assert reopened["scene"]["name"] == before["scene"]["name"]
        restored = client.call("faset_recovery_restore", {"document": identity, "revision": reopened["revision"]})
        assert restored["scene"] == changed["scene"]
        record("acknowledged unsaved edit survives killed Editor and explicit recovery")
        (project / "Scenes/blocked.scene.json").mkdir()
        client.call("faset_document_save", {"document": identity, "path": "Scenes/blocked.scene.json"}, error=True)
        assert client.call("faset_document_query", {"document": identity})["scene"] == restored["scene"]
        assert json.loads((project / "Scenes/main.scene.json").read_text())["name"] == before["scene"]["name"]
        record("failed save preserves live authoring and last saved file")
        bad = client.call("faset_scene_edit", {"document": identity, "revision": restored["revision"],
            "operations": [{"op": "entity.create", "entity": {
                "id": "future-version-object", "name": "Future component recovery fixture", "parent": None,
                "components": [{"id": "future-transform", "type": "faset.transform", "version": 999,
                                "fields": {"kept": True}}]}}]})
        play = client.call("faset_play", {"document": identity})
        client.job(play["job"])
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            logs = client.call("faset_editor_logs")["logs"]
            if any("Player exited with code 1" in line for line in logs):
                break
            time.sleep(.05)
        else:
            raise RuntimeError("Expected failed Player startup was not observed: " + str(logs))
        assert client.call("faset_document_query", {"document": identity})["scene"] == bad["scene"]
        record("failed Player process is reported without changing authoring data")
        restored = client.call("faset_undo", {"document": identity, "revision": bad["revision"]})
        assert restored["scene"] == changed["scene"]
        old_logs = len(client.call("faset_editor_logs")["logs"])
        client.job(client.call("faset_play", {"document": identity})["job"])
        time.sleep(2)
        logs = client.call("faset_editor_logs")["logs"][old_logs:]
        assert any("Play started" in line for line in logs), logs
        assert not any("Player exited with code" in line for line in logs), logs
        client.call("faset_stop")
        assert client.call("faset_document_query", {"document": identity})["scene"] == restored["scene"]
        record("Undo repairs the scene, then Play and Stop leave authoring unchanged")
        metadata = client.call("faset_schema")
        gameplay = project / "Scripts/Gameplay.cpp"
        original = gameplay.read_bytes()
        gameplay.write_bytes(original + b"\n#error intentional_recovery_acceptance_compile_failure\n")
        try:
            client.job(client.call("faset_build")["job"], "failed")
            assert client.call("faset_schema") == metadata
            assert client.call("faset_schema_status")["stale"]
            record("failed C++ build keeps prior schema and reports stale status")
        finally:
            gameplay.write_bytes(original)
        client.job(client.call("faset_build")["job"])
        assert not client.call("faset_schema_status")["stale"]
        record("fixed C++ rebuild restores current schema status")
        result = client.call("faset_import", {"path": "Assets/missing.glb"})
        client.job(result["job"], "failed")
        assert client.call("faset_document_query", {"document": identity})["scene"] == restored["scene"]
        record("failed import preserves authoring")
        (output / "report.json").write_text(json.dumps({"format": "faset.editor-recovery-verification",
            "version": 1, "status": "passed", "checks": checks,
            "recorded_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "platform": platform.platform(),
            "editor_sha256": hashlib.sha256(editor.read_bytes()).hexdigest(),
            "logs": client.call("faset_editor_logs")["logs"]}, indent=2) + "\n")
    finally:
        client.close()


if __name__ == "__main__":
    main()
