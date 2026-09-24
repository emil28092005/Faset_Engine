"""Contract and arithmetic tests for the offline P3 lighting sweep wrapper."""

import csv
import json
import hashlib
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tools/benchmark_p3_lighting.py"
sys.path.insert(0, str(ROOT / "tools"))


def sample(shadows, visibility, lights, raster, gpu, repeat=1, frame=0):
    requested = 6 * lights if shadows == "on" else 0
    rendered = min(12, requested)
    return {
        "shadows": shadows, "visibility": visibility, "light_count": str(lights),
        "run_index": str(repeat), "frame": str(frame),
        "gpu_main_raster_ms": str(raster), "gpu_post_raster_ms": "0",
        "gpu_post_visible": "0", "gpu_ms": str(gpu),
        "gpu_shadow_ms": "0", "cpu_ms": "1", "readback_cpu_ms": ".5",
        "validation_errors": "0", "device": "Fake GPU", "driver": "Fake Driver",
        "commit": "abc123", "effective_visibility": visibility,
        "lighting_path": "forward", "submitted_local_lights": str(lights),
        "omitted_local_lights": "0", "shadow_tiles": str(rendered), "draw_calls": "1",
        "gpu_bytes": "4096", "validation_enabled": "0", "width": "1920", "height": "1080",
        "build_configuration": "Release", "requested_local_shadow_faces": str(requested),
        "rendered_local_shadow_faces": str(rendered),
        "dropped_shadow_faces": str(requested - rendered),
        "shadow_atlas_full_drops": str(requested - rendered),
    }


def source_repository(root: Path) -> tuple[Path, str]:
    source = root / "source"
    source.mkdir()
    (source / "README").write_text("fixed source\n", encoding="utf-8")
    subprocess.run(["git", "init", "-q", source], check=True)
    subprocess.run(["git", "-C", source, "-c", "user.name=Benchmark Test",
                    "-c", "user.email=benchmark@example.invalid", "add", "README"], check=True)
    subprocess.run(["git", "-C", source, "-c", "user.name=Benchmark Test",
                    "-c", "user.email=benchmark@example.invalid", "commit", "-qm", "Fixture"],
                   check=True)
    revision = subprocess.check_output(["git", "-C", source, "rev-parse", "HEAD"],
                                       text=True).strip()
    return source, revision


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
              "shadow_tiles", "draw_calls", "gpu_bytes", "validation_enabled", "width", "height",
              "build_configuration", "requested_local_shadow_faces", "rendered_local_shadow_faces",
              "dropped_shadow_faces", "shadow_atlas_full_drops", "gpu_post_raster_ms",
              "gpu_post_visible"]
with path.open("w", newline="", encoding="utf-8") as stream:
    writer = csv.DictWriter(stream, fieldnames=fieldnames)
    writer.writeheader()
    for frame in range(a.frames):
        writer.writerow(dict(light_count=a.lights, shadows=a.shadows,
                             visibility=a.visibility, frame=frame, device="Fake GPU",
                             driver="Fake Driver", commit=a.commit,
                             gpu_main_raster_ms=.4 + .02 * a.lights, gpu_post_raster_ms=0,
                             gpu_post_visible=0, gpu_ms=4 + .02 * a.lights,
                             gpu_shadow_ms=0, cpu_ms=1, readback_cpu_ms=.5,
                             validation_errors=0, run_index=a.run_index,
                             effective_visibility=a.visibility, lighting_path="forward",
                             submitted_local_lights=a.lights, omitted_local_lights=0,
                             shadow_tiles=min(12, 6 * a.lights) if a.shadows == "on" else 0,
                             draw_calls=1, gpu_bytes=4096,
                             validation_enabled=0, width=a.width, height=a.height,
                             build_configuration="Release",
                             requested_local_shadow_faces=6 * a.lights if a.shadows == "on" else 0,
                             rendered_local_shadow_faces=min(12, 6 * a.lights) if a.shadows == "on" else 0,
                             dropped_shadow_faces=max(0, 6 * a.lights - 12) if a.shadows == "on" else 0,
                             shadow_atlas_full_drops=max(0, 6 * a.lights - 12) if a.shadows == "on" else 0))
'''

FAKE_EXPLICIT_BENCHMARK = (FAKE_BENCHMARK
    .replace('p.add_argument("--driver")',
             'p.add_argument("--driver")\np.add_argument("--lighting", required=True)')
    .replace('path = Path(a.csv)',
             'tile_ms = .1 if a.lighting == "tiled" and a.lights else 0\npath = Path(a.csv)')
    .replace('fieldnames = ["light_count"',
             'fieldnames = ["requested_lighting", "gpu_light_tiles_ms", '
             '"gpu_build_plus_raster_ms", "light_tile_count", "light_tile_counts_valid", '
             '"light_tile_candidate_count", "light_tile_overflow_count", "light_count"')
    .replace('lighting_path="forward",',
             'lighting_path="tiled" if tile_ms else "forward", '
             'requested_lighting=a.lighting, gpu_light_tiles_ms=tile_ms, '
             'gpu_build_plus_raster_ms=.4 + .02 * a.lights + tile_ms, '
             'light_tile_count=8160 if tile_ms else 0, light_tile_counts_valid=0, '
             'light_tile_candidate_count=0, light_tile_overflow_count=0,'))


class LightingBenchmarkTests(unittest.TestCase):
    def test_explicit_sweep_preserves_zero_light_fallback_and_full_matrix(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            fake = root / "explicit.py"
            fake.write_text(FAKE_EXPLICIT_BENCHMARK, encoding="utf-8")
            source, revision = source_repository(root)
            output = root / "tiled-output"
            process = subprocess.run(
                [sys.executable, SCRIPT, "--sweep", "--executable", fake,
                 "--output", output, "--shadows", "off", "--commit", revision,
                 "--source-root", source, "--driver", "Fake Driver",
                 "--lighting", "tiled"], capture_output=True, text=True)
            self.assertEqual(process.returncode, 0, process.stderr)
            report = json.loads((output / "summary.json").read_text(encoding="utf-8"))
            self.assertEqual((report["version"], report["requested_lighting"],
                              report["lighting_path"]), (2, "tiled", "mixed"))
            self.assertEqual((report["runs_completed"], report["rows"]), (54, 1620))
            self.assertIsNone(report["forward_plus_gate"])
            zero = next(row for row in report["configurations"]
                        if row["visibility"] == "direct" and row["light_count"] == 0)
            high = next(row for row in report["configurations"]
                        if row["visibility"] == "direct" and row["light_count"] == 32)
            self.assertAlmostEqual(zero["median_ms"]["gpu_light_tiles_ms"], 0)
            self.assertAlmostEqual(high["median_ms"]["gpu_light_tiles_ms"], .1)

    def test_explicit_tiled_sweep_accepts_zero_light_fallback_and_accounts_for_build(self):
        from benchmark_p3_lighting import _read_run_csv, summarize_rows

        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "tiled.csv"
            rows = [sample("off", "direct", 0, .4, 1.2, frame=frame)
                    for frame in range(30)]
            for row in rows:
                row.update(requested_lighting="tiled", gpu_light_tiles_ms="0",
                           gpu_build_plus_raster_ms=".4", light_tile_count="0",
                           light_tile_counts_valid="0", light_tile_candidate_count="0",
                           light_tile_overflow_count="0")
            with path.open("w", newline="", encoding="utf-8") as stream:
                writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
                writer.writeheader()
                writer.writerows(rows)
            run = {"shadows": "off", "visibility": "direct", "light_count": 0,
                   "repeat": 1}
            _, accepted = _read_run_csv(path, run, "abc123", "Fake Driver", "off", "tiled")
            self.assertEqual(len(accepted), 30)
            for row in rows:
                row.update(light_count="32", lighting_path="tiled",
                           submitted_local_lights="32", gpu_light_tiles_ms=".2",
                           gpu_build_plus_raster_ms=".7", gpu_main_raster_ms=".5",
                           light_tile_count="8160")
            with path.open("w", newline="", encoding="utf-8") as stream:
                writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
                writer.writeheader()
                writer.writerows(rows)
            run["light_count"] = 32
            _, accepted = _read_run_csv(path, run, "abc123", "Fake Driver", "off", "tiled")
            summary = summarize_rows(accepted, evaluate_forward_plus_gate=False)
            self.assertIsNone(summary["forward_plus_gate"])
            self.assertAlmostEqual(summary["configurations"][0]["median_ms"]
                                   ["gpu_build_plus_raster_ms"], .7)

    def test_explicit_tiled_sweep_rejects_fallback_and_wrong_build_time(self):
        from benchmark_p3_lighting import _read_run_csv

        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "invalid.csv"
            row = sample("off", "direct", 32, .5, 1.2)
            row.update(requested_lighting="tiled", gpu_light_tiles_ms=".2",
                       gpu_build_plus_raster_ms=".7", light_tile_count="8160",
                       light_tile_counts_valid="0", light_tile_candidate_count="0",
                       light_tile_overflow_count="0")
            def check(error):
                with path.open("w", newline="", encoding="utf-8") as stream:
                    writer = csv.DictWriter(stream, fieldnames=list(row))
                    writer.writeheader()
                    writer.writerows([{**row, "frame": str(frame)} for frame in range(30)])
                run = {"shadows": "off", "visibility": "direct", "light_count": 32,
                       "repeat": 1}
                with self.assertRaisesRegex(ValueError, error):
                    _read_run_csv(path, run, "abc123", "Fake Driver", "off", "tiled")
            check("lighting_path")
            row["lighting_path"] = "tiled"
            row["gpu_build_plus_raster_ms"] = ".5"
            check("gpu_build_plus_raster_ms")

    def test_compare_requires_identical_release_binary_and_uses_build_plus_raster(self):
        from benchmark_p3_lighting import compare_sweeps

        identity = dict(source_revision="abc123", benchmark_sha256="same-binary",
                        shader_bundle={"fragmentMain.spv": "same-shader"},
                        device="Fake GPU", driver="Fake Driver", validation="off",
                        width=1920, height=1080, warmup_frames_per_run=10,
                        measured_frames_per_run=30, build_configuration="Release",
                        runs_completed=1, rows=30)
        def entry(build_times):
            return dict(shadows="off", visibility="direct", light_count=32,
                        runs=[dict(run_index=repeat, frames=30,
                                   median_ms={"gpu_build_plus_raster_ms": value})
                              for repeat, value in enumerate(build_times, 1)],
                        median_ms={"gpu_build_plus_raster_ms": sorted(build_times)[1]})
        forward = dict(identity, requested_lighting="forward", lighting_path="forward",
                       configurations=[entry([1.0, 1.2, 1.1])])
        tiled = dict(identity, requested_lighting="tiled", lighting_path="mixed",
                     configurations=[entry([.8, .9, .7])])
        result = compare_sweeps(forward, tiled)
        self.assertAlmostEqual(result["comparisons"][0]["delta_tiled_minus_forward_ms"], -.3)
        self.assertAlmostEqual(result["comparisons"][0]["median_forward_ms"], 1.1)
        self.assertAlmostEqual(result["comparisons"][0]["median_tiled_ms"], .8)
        modified = dict(tiled, shader_bundle={"fragmentMain.spv": "different"})
        with self.assertRaisesRegex(ValueError, "shader_bundle"):
            compare_sweeps(forward, modified)

    def test_native_shader_bundle_manifest_tracks_loaded_spirv_and_reflection(self):
        from benchmark_p3_lighting import _shader_bundle_manifest

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            binary = root / "faset_p3_lighting_benchmark"
            binary.write_bytes(b"native fixture")
            shaders = root / "shaders"
            shaders.mkdir()
            with self.assertRaisesRegex(ValueError, "shader bundle"):
                _shader_bundle_manifest(binary)
            for name in ("vertexMain.spv", "fragmentMain.spv",
                         "fragmentMain.reflection.json", "tileBuild.spv"):
                (shaders / name).write_bytes(name.encode())
            before = _shader_bundle_manifest(binary)
            self.assertEqual(set(before), {"vertexMain.spv", "fragmentMain.spv",
                                           "fragmentMain.reflection.json", "tileBuild.spv"})
            (shaders / "tileBuild.spv").write_bytes(b"modified tile shader")
            self.assertNotEqual(_shader_bundle_manifest(binary), before)

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
        positions = {(run["shadows"], run["visibility"], run["repeat"], run["light_count"]): index
                     for index, run in enumerate(runs)}
        for run in runs:
            if run["light_count"] not in (32, 64, 128):
                continue
            baseline = positions[(run["shadows"], run["visibility"], run["repeat"], 0)]
            separation = positions[(run["shadows"], run["visibility"],
                                    run["repeat"], run["light_count"])] - baseline
            self.assertGreater(separation, 0)
            self.assertLessEqual(separation, 5,
                                 "Each expensive run needs a nearby control on the same repeat")

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

    def test_gate_pairs_controls_and_counts_occlusion_post_raster(self):
        from benchmark_p3_lighting import summarize_rows

        rows = []
        for repeat, baseline_main, candidate_main in ((1, .4, 1.5),
                                                       (2, 1.4, 1.4),
                                                       (3, 2.4, 3.5)):
            rows.append(sample("off", "direct", 0, baseline_main, 5, repeat))
            rows.append(sample("off", "direct", 32, candidate_main, 6, repeat))
            control = sample("off", "gpu-occlusion", 0, .4, 4, repeat)
            candidate = sample("off", "gpu-occlusion", 64, .5, 4.9, repeat)
            candidate["gpu_post_raster_ms"] = ".8"
            rows.extend((control, candidate))
        summary = summarize_rows(rows)
        hits = {(item["visibility"], item["light_count"]): item
                for item in summary["forward_plus_gate"]["candidates"]}
        self.assertAlmostEqual(hits[("direct", 32)]["overhead_ms"], 1.1)
        self.assertAlmostEqual(hits[("gpu-occlusion", 64)]["overhead_ms"], .9)
        self.assertEqual(hits[("gpu-occlusion", 64)]["raster_metric"], "main_plus_post")

    def test_shadow_summary_keeps_requested_rendered_and_dropped_faces(self):
        from benchmark_p3_lighting import summarize_rows

        rows = [sample("on", "direct", 0, .4, 4, repeat)
                for repeat in (1, 2, 3)]
        rows += [sample("on", "direct", 64, 1.1, 5, repeat)
                 for repeat in (1, 2, 3)]
        summary = summarize_rows(rows)
        high = next(item for item in summary["configurations"] if item["light_count"] == 64)
        self.assertEqual(high["shadow_counts"], {
            "requested_local_shadow_faces": 384,
            "rendered_local_shadow_faces": 12,
            "shadow_tiles": 12,
            "dropped_shadow_faces": 372,
            "shadow_atlas_full_drops": 372,
        })

    def test_sweep_runs_fake_executable_and_preserves_all_raw_frames(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            fake = root / "fake_benchmark.py"
            fake.write_text(FAKE_BENCHMARK, encoding="utf-8")
            source, revision = source_repository(root)
            output = root / "café 世界"
            process = subprocess.run(
                [sys.executable, SCRIPT, "--sweep", "--executable", fake,
                 "--output", output, "--shadows", "off", "--commit", revision,
                 "--source-root", source,
                 "--driver", "Fake Driver"],
                text=True, capture_output=True)
            self.assertEqual(process.returncode, 0, process.stderr)
            raw = list((output / "raw").glob("*.csv"))
            self.assertEqual(len(raw), 54)
            with (output / "merged.csv").open(newline="", encoding="utf-8") as stream:
                merged = list(csv.DictReader(stream))
            self.assertEqual(len(merged), 54 * 30)
            self.assertEqual({row["source_csv"] for row in merged},
                             {path.name for path in raw})
            self.assertEqual({int(row["acquisition_index"]) for row in merged}, set(range(54)))
            report = json.loads((output / "summary.json").read_text(encoding="utf-8"))
            self.assertEqual(report["runs_completed"], 54)
            self.assertEqual(report["rows"], 54 * 30)
            self.assertTrue(report["forward_plus_gate"]["triggered"])
            self.assertEqual(report["benchmark_sha256"], hashlib.sha256(fake.read_bytes()).hexdigest())
            self.assertIsNone(report["shader_bundle"])
            self.assertEqual(report["source_revision"], revision)
            self.assertFalse(report["source_dirty"])
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
            source, revision = source_repository(root)
            output = root / "invalid"
            process = subprocess.run(
                [sys.executable, SCRIPT, "--sweep", "--executable", fake,
                 "--output", output, "--shadows", "off", "--commit", revision,
                 "--source-root", source,
                 "--driver", "Fake Driver"],
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
            source, revision = source_repository(root)
            output = root / "fallback-output"
            process = subprocess.run(
                [sys.executable, SCRIPT, "--sweep", "--executable", fake,
                 "--output", output, "--shadows", "off", "--commit", revision,
                 "--source-root", source,
                 "--driver", "Fake Driver"],
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
            source, revision = source_repository(root)
            output = root / "wrong-count-output"
            process = subprocess.run(
                [sys.executable, SCRIPT, "--sweep", "--executable", fake,
                 "--output", output, "--shadows", "off", "--commit", revision,
                 "--source-root", source,
                 "--driver", "Fake Driver"],
                text=True, capture_output=True)
            self.assertNotEqual(process.returncode, 0)
            self.assertIn("submitted_local_lights", process.stderr)
            self.assertFalse((output / "summary.json").exists())

    def test_sweep_requires_known_driver_identity(self):
        from benchmark_p3_lighting import sweep
        with tempfile.TemporaryDirectory() as temporary:
            fake = Path(temporary) / "fake.py"
            fake.write_text(FAKE_BENCHMARK, encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "--driver"):
                sweep(fake, Path(temporary) / "out", "off", "abc123")

    def test_sweep_rejects_debug_binary_before_reporting_a_release_gate(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source, revision = source_repository(root)
            fake = root / "debug.py"
            fake.write_text(FAKE_BENCHMARK.replace(
                'build_configuration="Release"', 'build_configuration="Debug"'),
                encoding="utf-8")
            output = root / "debug-output"
            process = subprocess.run(
                [sys.executable, SCRIPT, "--sweep", "--executable", fake,
                 "--output", output, "--source-root", source, "--commit", revision,
                 "--driver", "Fake Driver", "--shadows", "off"],
                text=True, capture_output=True)
            self.assertNotEqual(process.returncode, 0)
            self.assertIn("Release", process.stderr)
            self.assertFalse((output / "summary.json").exists())

    def test_sweep_rejects_dirty_or_misidentified_source_revision(self):
        from benchmark_p3_lighting import sweep

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source, revision = source_repository(root)
            fake = root / "fake.py"
            fake.write_text(FAKE_BENCHMARK, encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "revision"):
                sweep(fake, root / "wrong-commit", "off", "incorrect", driver="Fake Driver",
                      source_root=source)
            (source / "README").write_text("edited after commit\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "dirty"):
                sweep(fake, root / "dirty", "off", revision, driver="Fake Driver",
                      source_root=source)

    def test_sweep_rejects_mixed_device_driver_path_or_validation(self):
        mutations = {
            "device": ('device="Fake GPU"', 'device="Other GPU" if a.lights else "Fake GPU"'),
            "driver": ('driver="Fake Driver"', 'driver="Wrong Driver"'),
            "lighting_path": ('lighting_path="forward"',
                              'lighting_path="forward_plus" if a.lights else "forward"'),
            "validation_enabled": ('validation_enabled=0', 'validation_enabled=1'),
        }
        for field, (before, after) in mutations.items():
            with self.subTest(field=field), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                source, revision = source_repository(root)
                fake = root / "changed.py"
                fake.write_text(FAKE_BENCHMARK.replace(before, after), encoding="utf-8")
                output = root / "changed-output"
                process = subprocess.run(
                    [sys.executable, SCRIPT, "--sweep", "--executable", fake,
                     "--output", output, "--source-root", source, "--commit", revision,
                     "--driver", "Fake Driver", "--shadows", "off"],
                    text=True, capture_output=True)
                self.assertNotEqual(process.returncode, 0)
                self.assertIn(field, process.stderr)
                self.assertFalse((output / "summary.json").exists())


def real_executable_smoke(executable: Path) -> None:
    from benchmark_p3_lighting import REQUIRED_COLUMNS

    choices = subprocess.run([executable, "--list-runs"], capture_output=True,
                             text=True, check=True)
    declared = json.loads(choices.stdout)
    if declared["lights"] != [0, 4, 16, 32, 64, 128] or len(declared["visibility"]) != 3:
        raise AssertionError("C++ executable and Python sweep matrix disagree")
    with tempfile.TemporaryDirectory() as directory:
        output = Path(directory) / "café 世界" / "smoke.csv"
        subprocess.run([executable, "--lights", "4", "--shadows", "off",
                        "--visibility", "direct", "--width", "64", "--height", "64",
                        "--warmup", "0", "--frames", "1", "--validation", "on",
                        "--commit", "smoke", "--driver", "smoke-driver",
                        "--csv", output], capture_output=True, text=True, check=True)
        with output.open(newline="", encoding="utf-8") as stream:
            reader = csv.DictReader(stream)
            columns, rows = reader.fieldnames or [], list(reader)
        if set(REQUIRED_COLUMNS) - set(columns) or len(rows) != 1:
            raise AssertionError("Real benchmark CSV lacks a complete single-frame row")
        row = rows[0]
        if (row["effective_visibility"] != "direct" or row["lighting_path"] != "forward" or
                row["submitted_local_lights"] != "4" or row["validation_errors"] != "0" or
                float(row["gpu_main_raster_ms"]) <= 0):
            raise AssertionError("Real benchmark did not report the measured lighting path")


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--real-executable":
        real_executable_smoke(Path(sys.argv[2]).resolve())
    else:
        unittest.main()
