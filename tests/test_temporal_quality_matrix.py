#!/usr/bin/env python3
"""Check the optional P3 capture matrix without starting Vulkan."""

import csv
import io
import subprocess
import sys


def main() -> None:
    completed = subprocess.run(
        [sys.argv[1], "--list-quality-runs"], capture_output=True, text=True, check=False)
    assert completed.returncode == 0, completed.stderr
    rows = list(csv.DictReader(io.StringIO(completed.stdout)))
    assert len(rows) == 171, len(rows)
    assert sum(int(row["frames"]) for row in rows) == 1386
    assert {row["visibility"] for row in rows} == {
        "direct", "gpu-frustum", "gpu-occlusion"}

    sequence_frames = {
        "wire-static": 16,
        "pan": 16,
        "moving-cube": 16,
        "door-background": 16,
        "door-open": 10,
        "cut": 2,
        "resize": 2,
        "ui-alpha": 2,
        "teleport": 2,
        "projection": 2,
        "view-switch": 2,
    }
    modes = {"off", "current", "taa", "upscale-current", "upscale"}
    for visibility in ("direct", "gpu-frustum", "gpu-occlusion"):
        actual = [row for row in rows if row["visibility"] == visibility]
        assert len(actual) == 57
        assert len({(row["sequence"], row["mode"]) for row in actual}) == 57
        for sequence, count in sequence_frames.items():
            runs = [row for row in actual
                    if row["sequence"] == sequence and row["mode"] != "spatial-2x"]
            assert {row["mode"] for row in runs} == modes
            assert all(int(row["frames"]) == count for row in runs)
            assert all((int(row["width"]), int(row["height"])) == (160, 120)
                       for row in runs)
        for sequence in ("wire-static", "pan"):
            reference = next(row for row in actual
                             if row["sequence"] == sequence and row["mode"] == "spatial-2x")
            assert (int(reference["frames"]), int(reference["width"]),
                    int(reference["height"])) == (16, 320, 240)


if __name__ == "__main__":
    main()
