"""Production Player reloads Lua atomically and keeps the old world on failure."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time


def behavior(marker, fail=False):
    start = 'error("LUA_RELOAD_START_FAILED")' if fail else f'faset.log("LUA_START_{marker}")'
    return f'''local behavior = faset.behavior {{
    id = "test.lua.reload", version = 1, name = "Reload test", fields = {{}}
}}
function behavior:on_start() {start} end
function behavior:on_destroy() faset.log("LUA_DESTROY_{marker}") end
return behavior
'''


with tempfile.TemporaryDirectory(prefix="faset-lua-reload-") as temporary:
    root = Path(temporary)
    (root / "Scripts").mkdir()
    (root / "project.faset.json").write_text(json.dumps({
        "format": "faset.project", "version": 1,
        "scripting": {"lua": {"scripts": ["Scripts/reload.lua"]}}
    }), encoding="utf-8")
    source = root / "Scripts/reload.lua"
    source.write_text(behavior("A"), encoding="utf-8")
    scene = root / "scene.scene.json"
    scene.write_text(json.dumps({
        "format": "faset.scene", "version": 1, "id": "reload-scene", "dimension": 2,
        "entities": [{"id": "object", "name": "Lua", "parent": None,
                      "components": [{"id": "lua", "type": "test.lua.reload",
                                      "version": 1, "fields": {}}]}]
    }), encoding="utf-8")
    control = root / "control.json"
    logs = []
    process = subprocess.Popen([sys.argv[1], "--scene", str(scene), "--project", str(root),
                                "--control", str(control), "--watch-lua", "--headless",
                                "--frames", "10000000"], stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True, encoding="utf-8")

    def collect():
        for line in process.stderr:
            logs.append(line)

    reader = threading.Thread(target=collect, daemon=True)
    reader.start()

    def wait_for(marker, count=1):
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            if "".join(logs).count(marker) >= count:
                return
            assert process.poll() is None, "".join(logs)
            time.sleep(.025)
        raise AssertionError((marker, "".join(logs)))

    def send(sequence, command):
        staging = root / "control.tmp"
        staging.write_text(json.dumps({"sequence": sequence, "command": command}),
                           encoding="utf-8")
        staging.replace(control)

    try:
        wait_for("LUA_START_A")
        source.write_text("invalid Lua syntax !", encoding="utf-8")
        wait_for("Lua reload rejected;")
        time.sleep(.65)
        assert "".join(logs).count("Lua reload rejected;") == 1, logs
        assert "LUA_DESTROY_A" not in "".join(logs), logs
        source.write_text(behavior("BAD", fail=True), encoding="utf-8")
        wait_for("LUA_RELOAD_START_FAILED")
        assert "LUA_DESTROY_A" not in "".join(logs), logs
        source.write_text(behavior("B"), encoding="utf-8")
        wait_for("LUA_START_B")
        assert "".join(logs).count("LUA_DESTROY_A") == 1, logs
        send(1, "reload-lua")
        wait_for("LUA_START_B", count=2)
        assert "".join(logs).count("LUA_DESTROY_B") == 1, logs
        send(2, "stop")
        process.wait(timeout=15)
        reader.join(timeout=2)
        assert process.returncode == 0, "".join(logs)
        assert "".join(logs).count("LUA_DESTROY_B") == 2, logs
        assert json.loads(process.stdout.read())["frames"] > 0
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=3)
        reader.join(timeout=2)
print("Lua Player watched/explicit reload, startup rollback and shutdown logs passed")
