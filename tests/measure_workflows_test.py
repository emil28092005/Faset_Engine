"""Contracts for reproducible, honestly labelled P1 workflow measurements."""

import importlib.util
from pathlib import Path
import unittest


MODULE = Path(__file__).resolve().parents[1] / "tools/measure_workflows.py"
SPEC = importlib.util.spec_from_file_location("measure_workflows", MODULE)
workflows = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(workflows)


def report(samples):
    return {
        "format": "faset.workflow-measurements",
        "version": 2,
        "revision": "0123456789abcdef",
        "source_hashes": {"Scripts/Gameplay.cpp": "a" * 64},
        "build_configuration": "Debug",
        "samples": samples,
    }


class WorkflowReportTests(unittest.TestCase):
    def test_nearest_rank_summary(self):
        self.assertEqual(workflows.summarize([1, 2, 3, 4, 5]),
                         {"median": 3, "p95": 5})

    def test_summary_rejects_empty_and_invalid_samples(self):
        for values in ([], [0, -1], [1, float("nan")]):
            with self.subTest(values=values), self.assertRaises(ValueError):
                workflows.summarize(values)

    def test_report_requires_revision_hashes_and_repetitions(self):
        samples = {"warm_unchanged_build": [
            {"seconds": 1.0, "build_configuration": "Debug", "source_hash": "a" * 64}
            for _ in range(5)]}
        workflows.validate_report(report(samples))
        for mutation in (
            lambda value: value.update(revision=""),
            lambda value: value.update(source_hashes={}),
            lambda value: value["samples"]["warm_unchanged_build"].pop(),
            lambda value: value["samples"]["warm_unchanged_build"][0].update(
                build_configuration="Release"),
        ):
            value = report({"warm_unchanged_build": [dict(sample) for sample in samples["warm_unchanged_build"]]})
            mutation(value)
            with self.subTest(value=value), self.assertRaises(ValueError):
                workflows.validate_report(value)

    def test_windowed_claim_rejects_offscreen_profile(self):
        value = report({"windowed_first_presented_frame": [{
            "seconds": 0.1, "build_configuration": "Debug",
            "source_hash": "a" * 64, "presentation_mode": "offscreen"}]})
        with self.assertRaises(ValueError):
            workflows.validate_report(value)

    def test_generated_scene_is_seeded_and_preserves_source(self):
        scene = {"format": "faset.scene", "version": 1, "entities": [{
            "id": "reference", "name": "Reference", "parent": None,
            "components": [{"id": "transform", "type": "faset.transform", "version": 1,
                            "fields": {"position": [0, 0, 0]}}]}]}
        first = workflows.generate_scene_fixture(scene, seed=17, count=8)
        second = workflows.generate_scene_fixture(scene, seed=17, count=8)
        self.assertEqual(first, second)
        self.assertEqual(len(first["entities"]), 9)
        self.assertEqual(scene["entities"][0]["id"], "reference")
        self.assertEqual(len({entry["id"] for entry in first["entities"]}), 9)
        self.assertNotEqual(first, workflows.generate_scene_fixture(scene, seed=18, count=8))

    def test_gnu_time_keeps_rss_after_expected_command_failure(self):
        self.assertEqual(workflows.parse_peak_rss(
            'Command exited with non-zero status 1\n{"peak_rss_kib":353272}\n'), 353272)
        with self.assertRaises(ValueError):
            workflows.parse_peak_rss("Command exited with non-zero status 1\n")


if __name__ == "__main__":
    unittest.main()
