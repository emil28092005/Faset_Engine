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
    lighting = report["samples"][0]
    assert lighting["effective_lighting_path"] == "forward", lighting
    for field in ["submitted_local_lights", "omitted_local_lights",
                  "requested_sun_cascades", "effective_sun_cascades",
                  "requested_local_shadow_faces", "local_shadow_faces",
                  "local_shadow_tiles", "dropped_shadow_faces",
                  "dropped_point_shadow_faces", "shadow_atlas_full_drops",
                  "shadow_caster_budget_drops", "shadow_unavailable_drops",
                  "shadow_caster_draws", "sun_shadow_atlas_bytes",
                  "local_shadow_atlas_bytes", "gpu_main_raster_ms",
                  "gpu_sun_shadow_ms", "gpu_local_shadow_ms",
                  "gpu_light_tiles_ms", "light_tile_count", "light_tile_counts_valid",
                  "light_tile_candidate_count", "light_tile_overflow_count"]:
        assert field in lighting, (field, lighting)
    assert lighting["submitted_local_lights"] == 0 and \
           lighting["effective_sun_cascades"] == 0 and \
           lighting["local_shadow_faces"] == 0, lighting

    for temporal_mode, scale in [("off", None), ("taa", None), ("upscale", "0.67")]:
        temporal_profile = root / f"temporal-{temporal_mode}.json"
        arguments = [sys.argv[1], "--scene", str(scene), "--headless", "--frames", "2",
                     "--temporal", temporal_mode, "--profile", str(temporal_profile)]
        if scale is not None:
            arguments += ["--render-scale", scale]
        selected = subprocess.run(arguments, capture_output=True, text=True,
                                  encoding="utf-8", timeout=30)
        assert selected.returncode == 0, (temporal_mode, selected.stdout, selected.stderr)
        temporal_report = json.loads(temporal_profile.read_text(encoding="utf-8"))
        assert temporal_report["temporal_mode"] == temporal_mode, temporal_report
        assert abs(temporal_report["render_scale"] -
                   (float(scale) if scale else 1.0)) < 0.000001
        assert temporal_report["effective_temporal_mode"] == temporal_mode, temporal_report
        for sample in temporal_report["samples"]:
            for key in ["requested_temporal_mode", "effective_temporal_mode",
                        "temporal_fallback_reason", "temporal_reset_reason",
                        "temporal_history_valid", "temporal_internal_width",
                        "temporal_internal_height", "temporal_jitter",
                        "gpu_temporal_resolve_ms", "gpu_temporal_composite_ms", "gpu_ui_ms"]:
                assert key in sample, (key, sample)
            assert sample["effective_temporal_mode"] == temporal_mode, sample
            if temporal_mode != "off":
                assert sample["temporal_internal_width"] > 0, sample
                assert sample["temporal_internal_height"] > 0, sample
                assert sample["gpu_temporal_resolve_ms"] is not None, sample
        if temporal_mode != "off":
            assert not temporal_report["samples"][0]["temporal_history_valid"]
            assert temporal_report["samples"][1]["temporal_history_valid"]

    for invalid in [["--temporal", "missing"], ["--render-scale", "NaN"],
                    ["--temporal", "taa", "--render-scale", "0.67"],
                    ["--temporal", "upscale", "--render-scale", "1.0"]]:
        result = subprocess.run([sys.argv[1], *invalid], capture_output=True,
                                text=True, encoding="utf-8", timeout=20)
        assert result.returncode != 0 and ("temporal" in result.stderr.lower() or
                                           "scale" in result.stderr.lower()), result

    # The same linked v2 schema must validate without registering or invoking behavior.
    validated = subprocess.run([sys.argv[1], "--scene", str(scene), "--validate"],
                               capture_output=True, text=True, encoding="utf-8", timeout=20)
    assert validated.returncode == 0, validated.stderr
    assert "TEST_" not in validated.stderr, validated.stderr
    assert json.loads(validated.stdout)["validated"]

    for mode, active in [("direct", False), ("gpu-frustum", True),
                         ("gpu-occlusion", True)]:
        mode_profile = root / f"{mode}.json"
        selected = subprocess.run([sys.argv[1], "--scene", str(scene), "--headless",
                                   "--frames", "2", "--visibility", mode,
                                   "--profile", str(mode_profile)],
                                  capture_output=True, text=True, encoding="utf-8", timeout=30)
        assert selected.returncode == 0, (mode, selected.stdout, selected.stderr)
        mode_report = json.loads(mode_profile.read_text(encoding="utf-8"))
        assert mode_report["visibility_mode"] == mode, mode_report
        assert mode_report["effective_visibility_mode"] == mode, mode_report
        assert all(sample["effective_visibility_mode"] == mode
                   for sample in mode_report["samples"]), mode_report["samples"]
        assert all(sample["gpu_visibility_active"] is active
                   for sample in mode_report["samples"]), mode_report["samples"]

    invalid_mode = subprocess.run([sys.argv[1], "--visibility", "missing"],
                                  capture_output=True, text=True, encoding="utf-8", timeout=20)
    assert invalid_mode.returncode != 0 and "visibility" in invalid_mode.stderr.lower(), invalid_mode
print("Player normal shutdown diagnostics, preserved tick/profile stats and callback-free validation passed")
