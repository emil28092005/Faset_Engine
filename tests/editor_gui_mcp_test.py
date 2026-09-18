"""Real native Editor + stdio MCP regression; requires a desktop and Vulkan.

Repeated fresh captures cover presentation back-pressure in hidden/occluded windows.
The test deliberately terminates its owned GUI process after assertions; closing a
client must not close the user's interactive Editor.
"""
import base64
import json
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading


def run(executable, project):
    process = subprocess.Popen(
        [executable, "--project", str(project), "--new", "GUI MCP acceptance",
         "--dimension", "3", "--mcp", "--gui"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True, encoding="utf-8")
    replies, diagnostics = queue.Queue(), []

    def read_stdout():
        try:
            for line in process.stdout:
                replies.put(json.loads(line))
        except Exception as error:
            replies.put(error)
        finally:
            replies.put(None)

    def read_stderr():
        for line in process.stderr:
            diagnostics.append(line)

    readers = [threading.Thread(target=read_stdout, daemon=True),
               threading.Thread(target=read_stderr, daemon=True)]
    for reader in readers:
        reader.start()
    sequence = 0

    def request(method, params):
        nonlocal sequence
        sequence += 1
        process.stdin.write(json.dumps({"jsonrpc": "2.0", "id": sequence,
                                        "method": method, "params": params}) + "\n")
        process.stdin.flush()
        response = replies.get(timeout=60)
        assert isinstance(response, dict) and response.get("id") == sequence, response
        assert "error" not in response, response
        return response["result"]

    def call(name, arguments=None, error=False):
        result = request("tools/call", {"name": name, "arguments": arguments or {}})
        assert result.get("isError", False) == error, result
        return result

    try:
        request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                "clientInfo": {"name": "faset-gui-acceptance", "version": "1"}})
        process.stdin.write('{"jsonrpc":"2.0","method":"notifications/initialized"}\n')
        process.stdin.flush()
        document = call("faset_documents")["structuredContent"]["documents"][0]
        edited = call("faset_scene_edit", {
            "document": document["id"], "revision": document["revision"],
            "operations": [{"op": "entity.create", "entity": {
                "id": "mcp-cube", "name": "Shared GUI and MCP cube 世界", "parent": None,
                "components": [
                    {"id": "cube-transform", "type": "faset.transform", "version": 1,
                     "fields": {"position": [0, 1, 0], "rotation": [0, 0, 0], "scale": [1, 1, 1]}},
                    {"id": "cube-mesh", "type": "faset.mesh", "version": 1,
                     "fields": {"primitive": "cube", "asset": "", "color": [0.65, 0.5, 0.85, 1]}}
                ]}}]})["structuredContent"]
        call("faset_scene_edit", {"document": document["id"], "revision": document["revision"],
                                   "operations": [{"op": "scene.rename", "name": "Stale"}]}, True)
        images = []
        for index in range(12):
            relative = ".faset/screenshots/full.png" if index == 0 else ".faset/screenshots/view.png"
            result = call("faset_editor_capture", {"path": relative, "viewport_only": index != 0})
            image = next(item for item in result["content"] if item["type"] == "image")
            data = base64.b64decode(image["data"], validate=True)
            assert data.startswith(b"\x89PNG\r\n\x1a\n") and len(data) > 1000
            assert (project / relative).read_bytes() == data
            images.append(data)
        assert call("faset_capabilities")["structuredContent"]["viewport_capture"]
        call("faset_editor_capture", {"path": "../outside.png"}, True)
        call("faset_editor_capture", {"path": "wrong.jpg"}, True)
        undone = call("faset_undo", {"document": document["id"],
                                     "revision": edited["revision"]})["structuredContent"]
        assert all(entity["id"] != "mcp-cube" for entity in undone["scene"]["entities"])
        assert process.poll() is None
    finally:
        process.stdin.close()
        process.terminate()
        try:
            process.wait(timeout=20)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=10)
        for reader in readers:
            reader.join(timeout=5)
        errors = "".join(diagnostics)
        assert "Validation Error" not in errors, errors


with tempfile.TemporaryDirectory(prefix="faset-gui-mcp-") as directory:
    run(sys.argv[1], Path(directory))
print("Native GUI + MCP shared authoring, revision conflict, Undo, 12 fresh PNG captures and continued responsiveness passed")
