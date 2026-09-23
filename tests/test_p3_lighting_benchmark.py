"""Contract and arithmetic tests for the offline P3 lighting sweep wrapper."""

import csv
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tools/benchmark_p3_lighting.py"
sys.path.insert(0, str(ROOT / "tools"))


def sample(shadows, visibility, lights, raster, gpu, repeat=1, frame=0):
    return {
        "shadows": shadows, "visibility": visibility, "light_count": str(lights),
        "run_index": str(repeat), "frame": str(frame),
        "gpu_main_raster_ms": str(raster), "gpu_ms": str(gpu),
        "gpu_shadow_ms": "0", "cpu_ms": "1", "readback_cpu_ms": ".5",
        "validation_errors": "0", "device": "Fake GPU", "driver": "Fake Driver",
        "commit": "abc123", "effective_visibility": visibility,
        "lighting_path": "forward", "submitted_local_lights": str(lights),
        "omitted_local_lights": "0", "shadow_tiles": "0", "draw_calls": "1",
        "gpu_bytes": "4096", "validation_enabled": "0", "width": "1920", "height": "1080",
    }


FAKE_BENCHMARK = r'''import argparse
import csv
import json
from pathlib import Path

p = argparse.ArgumentParser()
for flag in ("lights", "run-index", "width", "height", "warmup", "frames"):
    p.add_argument("--" + flag, type=int, required=True)
for flag in ("shadows", "visibility", "csv", "commit", "validation"):
    p.add_argument("--" + flag, required=True)
p.add_argument("--driver")
a = p.parse_args()
path = Path(a.csv)
path.with_suffix(".args.json").write_text(json.dumps(vars(a)), encoding="utf-8")
fieldnames = ["light_count", "shadows", "visibility", "frame", "device", "driver",
              "commit", "gpu_main_raster_ms", "gpu_ms", "gpu_shadow_ms", "cpu_ms",
              "readback_cpu_ms", "validation_errors", "run_index", "effective_visibility",
              "lighting_path", "submitted_local_lights", "omitted_local_lights",
              "shadow_tiles", "draw_calls", "gpu_bytes", "validation_enabled", "width", "height"]
with path.open("w", newline="", encoding="utf-8") as stream:
    writer = csv.DictWriter(stream, fieldnames=fieldnames)
    writer.writeheader()
    for frame in range(a.frames):
        writer.writerow(dict(light_count=a.lights, shadows=a.shadows,
                             visibility=a.visibility, frame=frame, device="Fake GPU",
                             driver="Fake Driver", commit=a.commit,
                             gpu_main_raster_ms=.4 + .02 * a.lights, gpu_ms=4 + .02 * a.lights,
                             gpu_shadow_ms=0, cpu_ms=1, readback_cpu_ms=.5,
                             validation_errors=0, run_index=a.run_index,
                             effective_visibility=a.visibility, lighting_path="forward",
                             submitted_local_lights=a.lights, omitted_local_lights=0,
                             shadow_tiles=0, draw_calls=1, gpu_bytes=4096,
                             validation_enabled=0, width=a.width, height=a.height))
'''


class LightingBenchmarkTests(unittest.TestCase):
    def test_list_runs_has_three_independent_repeats_for_each_shadow_setting(self):
        process = subprocess.run([sys.executable, SCRIPT, "--list-runs"],
                                 text=True, capture_output=True, check=True)
        runs = json.loads(process.stdout)["runs"]
        self.assertEqual(len(runs), 108)
        for shadows in ("off", "on"):
            group = [run for run in runs if run["shadows"] == shadows]
            self.assertEqual(len(group), 54)
            self.assertEqual({(run["light_count"], run["visibility"])
                              for run in group},
                             {(light, mode) for light in (0, 4, 16, 32, 64, 128)
                              for mode in ("direct", "gpu-frustum", "gpu-occlusion")})
            self.assertEqual({(run["light_count"], run["visibility"], run["repeat"])
                              for run in group},
                             {(light, mode, repeat)
                              for light in (0, 4, 16, 32, 64, 128)
                              for mode in ("direct", "gpu-frustum", "gpu-occlusion")
                              for repeat in (1, 2, 3)})

    def test_gate_uses_per_run_medians_and_same_mode_shadow_baseline(self):
        from benchmark_p3_lighting import summarize_rows

        rows = []
        for repeat in (1, 2, 3):
            for frame in (0, 1, 2):
                rows.append(sample("off", "direct", 0, .5, 10, repeat, frame))
                rows.append(sample("off", "direct", 32,
                                   100 if repeat == 3 else 1.5, 11, repeat, frame))
                rows.append(sample("on", "gpu-frustum", 0, .5, 4, repeat, frame))
                rows.append(sample("on", "gpu-frustum", 64, 1.1, 4.6, repeat, frame))
                rows.append(sample("off", "gpu-occlusion", 0, .5, 4, repeat, frame))
                rows.append(sample("off", "gpu-occlusion", 128, 1.09, 4.59, repeat, frame))
        summary = summarize_rows(rows)
        hits = {(item["shadows"], item["visibility"], item["light_count"]): item
                for item in summary["forward_plus_gate"]["candidates"]}
        self.assertEqual(set(hits), {("off", "direct", 32),
                                     ("on", "gpu-frustum", 64)})
        self.assertAlmostEqual(hits[("off", "direct", 32)]["overhead_ms"], 1.0)
        self.assertAlmostEqual(hits[("on", "gpu-frustum", 64)]["overhead_ms"], .6)
        self.assertEqual(hits[("off", "direct", 32)]["zero_light_gpu_ms"], 10)
        self.assertEqual(hits[("on", "gpu-frustum", 64)]["zero_light_gpu_ms"], 4)

    def test_sweep_runs_fake_executable_and_preserves_all_raw_frames(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            fake = root / "fake_benchmark.py"
            fake.write_text(FAKE_BENCHMARK, encoding="utf-8")
            output = root / "café 世界"
            process = subprocess.run(
                [sys.executable, SCRIPT, "--sweep", "--executable", fake,
                 "--output", output, "--shadows", "off", "--commit", "abc123"],
                text=True, capture_output=True)
            self.assertEqual(process.returncode, 0, process.stderr)
            raw = list((output / "raw").glob("*.csv"))
            self.assertEqual(len(raw), 54)
            with (output / "merged.csv").open(newline="", encoding="utf-8") as stream:
                merged = list(csv.DictReader(stream))
            self.assertEqual(len(merged), 54 * 30)
            self.assertEqual({row["source_csv"] for row in merged},
                             {path.name for path in raw})
            report = json.loads((output / "summary.json").read_text(encoding="utf-8"))
            self.assertEqual(report["runs_completed"], 54)
            self.assertEqual(report["rows"], 54 * 30)
            self.assertTrue(report["forward_plus_gate"]["triggered"])
            one = json.loads(next((output / "raw").glob("*.args.json")).read_text())
            self.assertEqual((one["width"], one["height"], one["warmup"], one["frames"]),
                             (1920, 1080, 10, 30))
            self.assertEqual(one["validation"], "off")

    def test_sweep_rejects_missing_gpu_raster_column(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            fake = root / "bad_benchmark.py"
            fake.write_text(FAKE_BENCHMARK.replace(
                '"gpu_main_raster_ms", "gpu_ms"', '"gpu_ms"').replace(
                'gpu_main_raster_ms=.4 + .02 * a.lights, ', ''), encoding="utf-8")
            output = root / "invalid"
            process = subprocess.run(
                [sys.executable, SCRIPT, "--sweep", "--executable", fake,
                 "--output", output, "--shadows", "off", "--commit", "abc123"],
                text=True, capture_output=True)
            self.assertNotEqual(process.returncode, 0)
            self.assertIn("gpu_main_raster_ms", process.stderr)
            self.assertFalse((output / "summary.json").exists())

    def test_sweep_rejects_visibility_fallback_as_a_mode_measurement(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            fake = root / "fallback.py"
            fake.write_text(FAKE_BENCHMARK.replace(
                'effective_visibility=a.visibility', 'effective_visibility="direct"'),
                encoding="utf-8")
            output = root / "fallback-output"
            process = subprocess.run(
                [sys.executable, SCRIPT, "--sweep", "--executable", fake,
                 "--output", output, "--shadows", "off", "--commit", "abc123"],
                text=True, capture_output=True)
            self.assertNotEqual(process.returncode, 0)
            self.assertIn("effective_visibility", process.stderr)
            self.assertFalse((output / "summary.json").exists())

    def test_sweep_rejects_a_scene_that_does_not_submit_requested_lights(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            fake = root / "wrong-count.py"
            fake.write_text(FAKE_BENCHMARK.replace(
                'submitted_local_lights=a.lights', 'submitted_local_lights=0'),
                encoding="utf-8")
            output = root / "wrong-count-output"
            process = subprocess.run(
                [sys.executable, SCRIPT, "--sweep", "--executable", fake,
                 "--output", output, "--shadows", "off", "--commit", "abc123"],
                text=True, capture_output=True)
            self.assertNotEqual(process.returncode, 0)
            self.assertIn("submitted_local_lights", process.stderr)
            self.assertFalse((output / "summary.json").exists())


if __name__ == "__main__":
    unittest.main()
