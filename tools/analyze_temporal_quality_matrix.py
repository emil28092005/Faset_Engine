#!/usr/bin/env python3
"""Package and measure the P3 Direct/P2 temporal image-quality matrix."""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path
from statistics import mean

import numpy as np
from PIL import Image, ImageDraw

from analyze_temporal_quality import image_error, quantile


VISIBILITIES = ("direct", "gpu-frustum", "gpu-occlusion")
MODES = ("off", "current", "taa", "upscale-current", "upscale")
LONG_SEQUENCES = ("wire-static", "pan", "moving-cube", "door-background")
RESET_SEQUENCES = ("cut", "teleport", "projection", "view-switch", "resize")
SEQUENCE_FRAMES = {**dict.fromkeys(LONG_SEQUENCES, 16), "door-open": 10,
                   **dict.fromkeys((*RESET_SEQUENCES, "ui-alpha"), 2)}
REGIONS = {
    "wire-static": (25, 12, 110, 95),
    "pan": (25, 12, 110, 95),
    "moving-cube": (35, 25, 90, 75),
    "door-edge": (65, 45, 30, 35),
    "door-center": (75, 55, 10, 10),
    "ui": (2, 2, 25, 12),
    "reset": (35, 25, 90, 70),
}


def box_2x(source: np.ndarray) -> np.ndarray:
    """Make the 160x120 spatial target from an unjittered 320x240 capture."""
    if source.shape != (240, 320, 3):
        raise ValueError(f"Unexpected 2x source shape: {source.shape}")
    return np.asarray(Image.fromarray(source, "RGB").resize(
        (160, 120), Image.Resampling.BOX))


def stable_convergence(samples: list[dict[str, float | int]]) -> int | None:
    """First post-open frame whose remaining observed edge stays below the gate."""
    def below_gate(sample: dict[str, float | int]) -> bool:
        return sample["pixels_over_8"] <= 32 and sample["mean_rgb_255"] <= .75

    return next((index for index in range(len(samples))
                 if all(below_gate(sample) for sample in samples[index:])), None)


def package(input_dir: Path, output_dir: Path, revision: str, driver: str) -> None:
    if output_dir.exists():
        raise ValueError(f"Evidence output already exists; choose a new directory: {output_dir}")
    with (input_dir / "frames.csv").open(newline="", encoding="utf-8") as source:
        reader = csv.DictReader(source)
        rows = list(reader)
        columns = reader.fieldnames
    if not columns or len(rows) != 1386:
        raise ValueError(f"Expected exactly 1386 matrix frames, found {len(rows)}")
    if len({row["device"] for row in rows}) != 1:
        raise ValueError("Capture mixed Vulkan devices")
    if any(row["effective_visibility"] != row["visibility"] or
           int(row["validation_enabled"]) != 1 or
           int(row["validation_errors"]) != 0 for row in rows):
        raise ValueError("Visibility fallback or inactive/failing Vulkan validation")
    keys = [(row["visibility"], row["sequence"], row["mode"], int(row["phase"]))
            for row in rows]
    if len(set(keys)) != len(rows):
        raise ValueError("Capture contains duplicate phase keys")
    expected = {(visibility, sequence, mode, phase)
                for visibility in VISIBILITIES
                for sequence, count in SEQUENCE_FRAMES.items()
                for mode in MODES for phase in range(count)}
    expected.update((visibility, sequence, "spatial-2x", phase)
                    for visibility in VISIBILITIES
                    for sequence in ("wire-static", "pan") for phase in range(16))
    if set(keys) != expected:
        raise ValueError("Capture does not match the declared quality matrix")

    output_dir.mkdir(parents=True)
    images: dict[tuple[str, str, str, int], np.ndarray] = {}
    for row in rows:
        relative = Path(row["image"])
        if not relative.parts or relative.parts[0] != "captures" or ".." in relative.parts:
            raise ValueError(f"Unsafe capture path: {relative}")
        destination = output_dir / relative.with_suffix(".png")
        destination.parent.mkdir(parents=True, exist_ok=True)
        with Image.open(input_dir / relative) as source:
            if source.mode != "RGB" or source.size != (int(row["width"]), int(row["height"])):
                raise ValueError(f"Unexpected capture format or extent: {relative}")
            images[row["visibility"], row["sequence"], row["mode"],
                   int(row["phase"])] = np.asarray(source)
            source.save(destination, optimize=True)
        row["image"] = destination.relative_to(output_dir).as_posix()
    with (output_dir / "frames.csv").open("w", newline="", encoding="utf-8") as target:
        writer = csv.DictWriter(target, fieldnames=columns)
        writer.writeheader()
        writer.writerows(rows)

    report = {
        "format": "faset.p3-temporal-quality-matrix-v1",
        "source_revision": revision,
        "device": rows[0]["device"],
        "driver": driver,
        "capture_configuration": "Linux Debug; Vulkan validation enabled; 160x120 output "
                                 "except 319x241 resize and 320x240 spatial-2x source",
        "spatial_reference_method": "Off at 320x240, 2x2 box-filtered in display-encoded RGB "
                                    "to 160x120; it is a bounded 4-sample spatial reference",
        "roi_xywh": REGIONS,
        "frame_count": len(rows),
        "paths": {},
        "cross_visibility": {},
    }

    for visibility in VISIBILITIES:
        def image(sequence: str, mode: str, phase: int) -> np.ndarray:
            return images[visibility, sequence, mode, phase]

        path = {"variation": {}, "spatial_reference": {}, "door_reveal": {},
                "resets": {}, "ui": {}}
        for sequence in ("wire-static", "pan", "moving-cube"):
            variation = {}
            for mode in MODES:
                samples = [image_error(image(sequence, mode, phase),
                                       image(sequence, mode, phase - 1),
                                       REGIONS[sequence])["mean_rgb_255"]
                           for phase in range(5, 16)]
                variation[mode] = {"mean_rgb_frame_delta": mean(samples),
                                   "p95_rgb_frame_delta": quantile(samples, .95),
                                   "samples": samples}
            path["variation"][sequence] = variation

        for sequence in ("wire-static", "pan"):
            reference = {phase: box_2x(image(sequence, "spatial-2x", phase))
                         for phase in range(4, 16)}
            comparison = {}
            for mode in MODES:
                samples = [image_error(image(sequence, mode, phase), reference[phase],
                                       REGIONS[sequence]) for phase in range(4, 16)]
                comparison[mode] = {
                    "mean_rgb_error_255": mean(sample["mean_rgb_255"] for sample in samples),
                    "p95_rgb_error_255": quantile(
                        [sample["mean_rgb_255"] for sample in samples], .95),
                    "max_channel_255": max(sample["max_channel_255"] for sample in samples),
                    "per_phase": samples,
                }
            path["spatial_reference"][sequence] = comparison

        for mode in ("taa", "upscale"):
            current = "current" if mode == "taa" else "upscale-current"
            per_phase = []
            for phase in range(2, 10):
                rendered = image("door-open", mode, phase)
                prior_door_red = rendered[40:85, 60:100, :3].astype(np.int16)
                red_pixels = int(np.logical_and.reduce((
                    prior_door_red[:, :, 0] > prior_door_red[:, :, 1] + 20,
                    prior_door_red[:, :, 0] > prior_door_red[:, :, 2] + 20,
                    prior_door_red[:, :, 0] > 25)).sum())
                per_phase.append({
                    "phase": phase,
                    "edge_vs_unobstructed": image_error(
                        rendered, image("door-background", mode, phase),
                        REGIONS["door-edge"]),
                    "edge_vs_current_only": image_error(
                        rendered, image("door-open", current, phase),
                        REGIONS["door-edge"]),
                    "center_vs_current_only": image_error(
                        rendered, image("door-open", current, phase),
                        REGIONS["door-center"]),
                    "old_door_red_pixels": red_pixels,
                })
            path["door_reveal"][mode] = {
                "per_open_frame": per_phase,
                "observed_frames_to_stable_edge_gate": stable_convergence(
                    [sample["edge_vs_unobstructed"] for sample in per_phase]),
            }

        for sequence in RESET_SEQUENCES:
            path["resets"][sequence] = {}
            for mode in ("taa", "upscale"):
                current = "current" if mode == "taa" else "upscale-current"
                path["resets"][sequence][mode] = image_error(
                    image(sequence, mode, 1), image(sequence, current, 1),
                    REGIONS["reset"])
        path["ui"] = {mode: image_error(image("ui-alpha", mode, 1),
                                          image("ui-alpha", "off", 1), REGIONS["ui"])
                      for mode in MODES[1:]}
        report["paths"][visibility] = path

    for visibility in VISIBILITIES[1:]:
        samples = []
        for sequence, count in SEQUENCE_FRAMES.items():
            for mode in MODES:
                for phase in range(count):
                    first = images["direct", sequence, mode, phase]
                    second = images[visibility, sequence, mode, phase]
                    samples.append(image_error(first, second,
                                               (0, 0, first.shape[1], first.shape[0])))
        report["cross_visibility"][visibility] = {
            "compared_frames": len(samples),
            "mean_rgb_error_255": mean(s["mean_rgb_255"] for s in samples),
            "max_channel_255": max(s["max_channel_255"] for s in samples),
            "frames_with_pixels_over_8": sum(s["pixels_over_8"] > 0 for s in samples),
        }

    (output_dir / "metrics.json").write_text(
        json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

    # The nearest-neighbor enlargement makes the one-pixel reveal residual visible.
    sheet = Image.new("RGB", (8 * 170, 3 * 200), "#111111")
    draw = ImageDraw.Draw(sheet)
    for row_index, mode in enumerate(("current", "taa", "upscale")):
        for column_index, phase in enumerate(range(2, 10)):
            x, y = column_index * 170, row_index * 200
            crop = Image.fromarray(images["direct", "door-open", mode, phase], "RGB")
            crop = crop.crop((65, 45, 95, 80)).resize((120, 140), Image.Resampling.NEAREST)
            sheet.paste(crop, (x + 10, y + 25))
            draw.text((x + 5, y + 4), f"{mode} / open+{phase - 2}", fill="#eeeeee")
    sheet.save(output_dir / "door-trail-nearest-4x.png", optimize=True)

    reference_sheet = Image.new("RGB", (6 * 160, 2 * 144), "#111111")
    reference_draw = ImageDraw.Draw(reference_sheet)
    for row_index, sequence in enumerate(("wire-static", "pan")):
        for column_index, mode in enumerate((*MODES, "spatial-2x")):
            x, y = column_index * 160, row_index * 144
            frame = images["direct", sequence, mode, 15]
            if mode == "spatial-2x":
                frame = box_2x(frame)
            reference_sheet.paste(Image.fromarray(frame, "RGB"), (x, y + 24))
            reference_draw.text((x + 5, y + 4), f"{sequence} / {mode}", fill="#eeeeee")
    reference_sheet.save(output_dir / "spatial-reference.png", optimize=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--revision", required=True)
    parser.add_argument("--driver", required=True)
    args = parser.parse_args()
    package(args.input, args.output, args.revision, args.driver)
