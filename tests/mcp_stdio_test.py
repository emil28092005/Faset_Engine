"""Exercise the real headless Editor over newline-delimited MCP, without a GPU."""
import json
import pathlib
import queue
import subprocess
import sys
import tempfile
import threading


def run(executable, project):
    process = subprocess.Popen([executable, "--project", str(project), "--mcp"],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True, encoding="utf-8")
    output = queue.Queue()
    def reader():
        for line in process.stdout:
            output.put(json.loads(line))  # Any non-JSON stdout fails the test.
        output.put(None)
    threading.Thread(target=reader, daemon=True).start()
    sequence = 0
    def request(method, params=None):
        nonlocal sequence
        sequence += 1
        message = {"jsonrpc": "2.0", "id": sequence, "method": method}
        if params is not None:
            message["params"] = params
        process.stdin.write(json.dumps(message) + "\n")
        process.stdin.flush()
        result = output.get(timeout=20)
        assert result and result["id"] == sequence, result
        return result
    def call(name, arguments=None, error=False):
        response = request("tools/call", {"name": name, "arguments": arguments or {}})["result"]
        assert response["isError"] == error, response
        return response["structuredContent"]
    try:
        initialized = request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
            "clientInfo": {"name": "faset-integration-test", "version": "1"}})
        assert initialized["result"]["serverInfo"]["name"] == "faset-editor"
        process.stdin.write('{"jsonrpc":"2.0","method":"notifications/initialized"}\n')
        process.stdin.flush()
        names = {tool["name"] for tool in request("tools/list")["result"]["tools"]}
        assert {"faset_scene_edit", "faset_export", "faset_job_cancel", "faset_plugins"} <= names
        assert "faset_runtime_query" not in names and "faset_editor_capture" not in names
        if (project / "Scenes/main.scene.json").exists():
            opened = call("faset_document_open", {"path": "Scenes/main.scene.json"})
            assert opened["scene"]["entities"][0]["name"] == "Door 世界"
            return
        scene = call("faset_document_create", {"name": "MCP integration", "dimension": 3})
        identity = scene["id"]
        edit = {"document": identity, "revision": 0,
                "operations": [{"op": "entity.create", "name": "Door 世界"}],
                "idempotency_key": "create-door"}
        changed = call("faset_scene_edit", edit)
        assert call("faset_scene_edit", edit) == changed
        conflict = dict(edit)
        del conflict["idempotency_key"]
        assert call("faset_scene_edit", conflict, True)["error"]["code"] == "revision.conflict"
        undone = call("faset_undo", {"document": identity, "revision": 1})
        assert undone["scene"]["entities"] == []
        redone = call("faset_redo", {"document": identity, "revision": 2})
        assert redone["scene"] == changed["scene"]
        saved = call("faset_document_save", {"document": identity, "path": "Scenes/main.scene.json"})
        assert not saved["dirty"]
        call("faset_document_open", {"path": "../outside.json"}, True)
        assert request("resources/read", {"uri": "faset://documents"})["result"]["contents"]
        assert request("faset_runtime_query")["error"]["code"] == -32601
    finally:
        process.stdin.close()
        try:
            code = process.wait(timeout=20)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            raise
        errors = process.stderr.read()
        assert code == 0, errors


with tempfile.TemporaryDirectory(prefix="faset-mcp-stdio-") as temporary:
    run(sys.argv[1], pathlib.Path(temporary))
    run(sys.argv[1], pathlib.Path(temporary))
print("Real MCP stdio lifecycle, clean stdout, revision conflict, retry, Undo/Redo, disk reopen and process shutdown passed")
