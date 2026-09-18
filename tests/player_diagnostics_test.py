"""Normal Player shutdown reports lifecycle failures without losing completed stats."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile


with tempfile.TemporaryDirectory(prefix="faset-player-diagnostics-") as temporary:
    root = Path(temporary) / "Faset Café 世界"
    root.mkdir()
    scene = root / "Сцена.scene.json"
    profile = root / "Профиль.json"
    scene.write_text(json.dumps({
        "format": "faset.scene", "version": 1, "id": "diagnostics-scene",
        "name": "Lifecycle diagnostics", "dimension": 2, "instances": [],
        "entities": [{"id": "diagnostics-object", "name": "Throwing behavior", "parent": None,
                      "components": [{"id": "diagnostics-component", "type": "test.diagnostics",
                                      "version": 2, "fields": {}}]}]
    }), encoding="utf-8")
    result = subprocess.run([sys.argv[1], "--scene", str(scene), "--headless", "--frames", "1",
                             "--profile", str(profile)],
                            capture_output=True, text=True, encoding="utf-8", timeout=30)
    assert result.returncode == 0, (result.stdout, result.stderr)
    for phase in ["TEST_ON_START", "TEST_UPDATE", "TEST_ON_DESTROY"]:
        assert result.stderr.count(phase) == 1, (phase, result.stderr)
    summary = json.loads(result.stdout)
    assert summary["frames"] == 1 and summary["ticks"] == 1, summary
    report = json.loads(profile.read_text(encoding="utf-8"))
    assert report["completed_frames"] == 1 and len(report["samples"]) == 1, report
    assert report["samples"][0]["tick"] == 1, report["samples"]

    # The same linked v2 schema must validate without registering or invoking behavior.
    validated = subprocess.run([sys.argv[1], "--scene", str(scene), "--validate"],
                               capture_output=True, text=True, encoding="utf-8", timeout=20)
    assert validated.returncode == 0, validated.stderr
    assert "TEST_" not in validated.stderr, validated.stderr
    assert json.loads(validated.stdout)["validated"]
print("Player normal shutdown diagnostics, preserved tick/profile stats and callback-free validation passed")
