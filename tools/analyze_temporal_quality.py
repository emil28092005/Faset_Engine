#!/usr/bin/env python3
"""Package reproducible P3 temporal captures and compute fixed-fixture image metrics."""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path
from statistics import mean

import numpy as np
from PIL import Image, ImageDraw


REGIONS = {
    "wire-static": (25, 12, 110, 95),
    "pan": (25, 12, 110, 95),
    "moving-cube": (35, 25, 90, 75),
    "door-open": (75, 55, 10, 10),
    "cut": (35, 25, 90, 70),
    "resize": (45, 30, 70, 55),
    "ui-alpha": (2, 2, 25, 12),
}
MODES = ("off", "current", "taa", "upscale-current", "upscale")


def quantile(samples: list[float], fraction: float) -> float:
    ordered = sorted(samples)
    index = (len(ordered) - 1) * fraction
    lower = int(index)
    upper = min(lower + 1, len(ordered) - 1)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (index - lower)


def image_error(first: np.ndarray, second: np.ndarray,
                region: tuple[int, int, int, int]) -> dict[str, float | int]:
    x, y, width, height = region
    first_roi = first[y:y + height, x:x + width, :3].astype(np.int16)
    second_roi = second[y:y + height, x:x + width, :3].astype(np.int16)
    difference = np.abs(first_roi - second_roi)
    return {
        "mean_rgb_255": float(difference.mean()),
        "max_channel_255": int(difference.max()),
        "pixels_over_8": int(np.any(difference > 8, axis=2).sum()),
    }


def package(input_dir: Path, output_dir: Path, revision: str, driver: str) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / "captures").mkdir(exist_ok=True)
    with (input_dir / "frames.csv").open(newline="", encoding="utf-8") as source:
        reader = csv.DictReader(source)
        rows = list(reader)
        fieldnames = reader.fieldnames
    if not rows or not fieldnames:
        raise ValueError("Temporal capture CSV is empty")
    if len({row["device"] for row in rows}) != 1:
        raise ValueError("Capture mixed Vulkan devices")
    if any(int(row["validation_errors"]) != 0 for row in rows):
        raise ValueError("Capture contains Vulkan validation errors")

    images: dict[tuple[str, str, int], np.ndarray] = {}
    by_sequence: dict[tuple[str, str], list[dict[str, str]]] = {}
    for row in rows:
        ppm = input_dir / row["image"]
        png = Path(row["image"]).with_suffix(".png")
        with Image.open(ppm) as source:
            source.save(output_dir / png, optimize=True)
            images[row["sequence"], row["mode"], int(row["phase"])] = np.array(source)
        row["image"] = png.as_posix()
        by_sequence.setdefault((row["sequence"], row["mode"]), []).append(row)
    with (output_dir / "frames.csv").open("w", newline="", encoding="utf-8") as target:
        writer = csv.DictWriter(target, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)

    variation = {}
    for sequence in ("wire-static", "pan", "moving-cube"):
        variation[sequence] = {}
        for mode in MODES:
            sequence_rows = by_sequence[sequence, mode]
            samples = [image_error(images[sequence, mode, phase],
                                   images[sequence, mode, phase - 1],
                                   REGIONS[sequence])["mean_rgb_255"]
                       for phase in range(5, len(sequence_rows))]
            variation[sequence][mode] = {
                "mean_rgb_frame_delta": mean(samples),
                "p95_rgb_frame_delta": quantile(samples, .95),
                "samples": samples,
            }

    paired_errors = {}
    for sequence, phases in (("door-open", (2, 3)), ("cut", (1,)),
                             ("resize", (1,)), ("ui-alpha", (1,))):
        paired_errors[sequence] = {}
        for phase in phases:
            paired_errors[sequence][str(phase)] = {}
            for mode in MODES[1:]:
                baseline = "upscale-current" if mode == "upscale" else "current"
                paired_errors[sequence][str(phase)][mode] = {
                    "versus_off": image_error(images[sequence, mode, phase],
                                              images[sequence, "off", phase],
                                              REGIONS[sequence]),
                    "versus_current_only": image_error(images[sequence, mode, phase],
                                                       images[sequence, baseline, phase],
                                                       REGIONS[sequence]),
                }

    wire_energy = {}
    x, y, region_width, region_height = REGIONS["wire-static"]
    for mode in MODES:
        images_after_warmup = [images["wire-static", mode, phase]
                               [y:y + region_height, x:x + region_width, :3]
                               for phase in range(4, 16)]
        wire_energy[mode] = {
            "mean_rgb_sum": mean(float(image.sum()) for image in images_after_warmup),
            "mean_frame_peak": mean(float(image.max()) for image in images_after_warmup),
            "mean_pixels_over_128": mean(float(np.any(image > 128, axis=2).sum())
                                         for image in images_after_warmup),
        }
    door_edge = {}
    for phase in (2, 3):
        door_edge[str(phase)] = {}
        for mode in ("taa", "upscale"):
            baseline = "current" if mode == "taa" else "upscale-current"
            rendered = images["door-open", mode, phase]
            old_door = rendered[40:85, 60:100, :3].astype(np.int16)
            red_pixels = np.logical_and(
                np.logical_and(old_door[:, :, 0] > old_door[:, :, 1] + 20,
                               old_door[:, :, 0] > old_door[:, :, 2] + 20),
                old_door[:, :, 0] > 25).sum()
            door_edge[str(phase)][mode] = {
                "edge_versus_current_only": image_error(
                    rendered, images["door-open", baseline, phase],
                    (65, 45, 30, 35)),
                "edge_versus_unobstructed_temporal": image_error(
                    rendered, images["door-background", mode, phase],
                    (65, 45, 30, 35)),
                "old_door_red_pixels": int(red_pixels),
            }

    timing = {}
    columns = ("cpu_ms", "gpu_ms", "readback_cpu_ms", "gpu_main_raster_ms",
               "gpu_post_raster_ms", "gpu_temporal_resolve_ms",
               "gpu_temporal_composite_ms", "gpu_ui_ms", "gpu_allocated_bytes")
    for mode in MODES:
        steady = by_sequence["wire-static", mode][4:]
        timing[mode] = {
            column: {"p50": quantile([float(row[column]) for row in steady], .5),
                     "p95": quantile([float(row[column]) for row in steady], .95)}
            for column in columns
        }
    report = {
        "format": "faset.p3-temporal-quality",
        "source_revision": revision,
        "device": rows[0]["device"],
        "driver": driver,
        "capture_configuration": "Linux Debug, validation on, Direct, 160x120 except resize 319x241",
        "roi_xywh": REGIONS,
        "variation": variation,
        "wire_brightness": wire_energy,
        "door_edge": door_edge,
        "paired_errors": paired_errors,
        "wire_static_timing_after_four_warmup_frames": timing,
        "frame_count": len(rows),
    }
    (output_dir / "metrics.json").write_text(
        json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

    examples = (("wire-static", 15), ("pan", 15), ("moving-cube", 15),
                ("door-open", 2), ("cut", 1), ("ui-alpha", 1))
    cell_width, cell_height = 160, 144
    sheet = Image.new("RGB", (cell_width * len(MODES),
                              cell_height * len(examples)), "#111111")
    draw = ImageDraw.Draw(sheet)
    for row_index, (sequence, phase) in enumerate(examples):
        for column_index, mode in enumerate(MODES):
            x, y = column_index * cell_width, row_index * cell_height
            picture = Image.fromarray(images[sequence, mode, phase], "RGB")
            sheet.paste(picture, (x, y + 24))
            draw.text((x + 5, y + 4), f"{sequence} / {mode}", fill="#eeeeee")
    sheet.save(output_dir / "contact-sheet.png", optimize=True)

    door_modes = ("current", "taa", "upscale-current", "upscale")
    door_sheet = Image.new("RGB", (180 * len(door_modes), 210 * 2), "#111111")
    door_draw = ImageDraw.Draw(door_sheet)
    for row_index, sequence in enumerate(("door-open", "door-background")):
        for column_index, mode in enumerate(door_modes):
            x, y = column_index * 180, row_index * 210
            crop = Image.fromarray(images[sequence, mode, 3], "RGB").crop(
                (65, 45, 95, 80)).resize((150, 175), Image.Resampling.NEAREST)
            door_sheet.paste(crop, (x, y + 25))
            door_draw.text((x + 5, y + 4), f"{sequence} / {mode}", fill="#eeeeee")
    door_sheet.save(output_dir / "door-edge-nearest-5x.png", optimize=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--revision", required=True)
    parser.add_argument("--driver", required=True)
    args = parser.parse_args()
    package(args.input, args.output, args.revision, args.driver)
