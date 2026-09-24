# P3 integrated Forward+ decision on the P1+P3 renderer checkpoint

The P1+P3 integration keeps `Auto` on the simple forward light loop. On the
fixed, dense 1920 × 1080 scene, the complete tile-build-plus-raster work is
slower at every nonzero light count in all three visibility modes and both
shadow states. Explicit `Tiled` remains available: when the same lights have a
shorter range, it wins in the affected passes. This is a measured choice for the
recorded fixture, not a universal performance claim or an automatic light-
occupancy heuristic.

This is the **combined final-code repeat** of [the initial 32-light gate](22-p3-lighting-benchmark-2026-09-24.md)
and [the Forward+ implementation experiment](23-p3-forward-plus-2026-09-24.md).
Both A/B paths used clean source revision
`4a3453e2b1868555d833f578d43f48b8bc47d41a`, the same Release
benchmark executable SHA-256
`ceb4fbde8f65bad60a3eb4d24c917619769ff058c66851bc10748ef0af5b45fd`,
and an unchanged, per-file hashed shader bundle. This revision contains the
integrated temporal shader and P1 build changes, although temporal was **off**
in this lighting benchmark. The GPU was a physical NVIDIA GeForce RTX 2080 Ti,
Linux driver 595.84, with Vulkan validation disabled for timings.

## Method and results

Each path ran 36 configurations: shadows on/off × Direct/GPU frustum/GPU
occlusion × 0/4/16/32/64/128 lights. There were three fresh-process repeats
per configuration, ten warm-up plus thirty measured frames per repeat: **108
runs and 3,240 measured frame rows per path**, 6,480 rows total. The wrapper
checked clean source, binary and shader hashes, actual Vulkan device, requested
lighting and visibility path, light counts, per-frame GPU timestamps, shadow
face counts, and zero reported validation errors. Zero errors in these
validation-off sweeps is not a validation-layer result. Values below are the
median of the three process medians; the change is the paired median difference
reported by the comparison tool. Units are GPU milliseconds, 1920 × 1080.

| Shadows | Dense lights | Forward build + raster | Tile build | Tiled raster | Tiled build + raster | Tiled change |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Off | 32 | 0.5519 | 0.0621 | 0.5541 | 0.6161 | +0.0642 (+11.6%) |
| Off | 64 | 1.0653 | 0.1185 | 1.0784 | 1.1968 | +0.1321 (+12.4%) |
| Off | 128 | 2.1048 | 0.2240 | 2.1223 | 2.3460 | +0.2402 (+11.4%) |
| On | 32 | 0.5811 | 0.0622 | 0.5898 | 0.6519 | +0.0712 (+12.2%) |
| On | 64 | 1.1052 | 0.1189 | 1.1158 | 1.2351 | +0.1301 (+11.8%) |
| On | 128 | 2.1509 | 0.2242 | 2.1688 | 2.3924 | +0.2414 (+11.2%) |

The other two visibility modes follow the same direction. Across all three
modes, the 32-light dense penalty spans +0.0642–0.0710 ms with shadows off
and +0.0688–0.0720 ms with shadows on; at 128 lights it spans
+0.2402–0.2425 ms off and +0.2343–0.2420 ms on. The [36-configuration
comparison](data/p3-combined-2026-09-24/comparison.json) holds every point
and paired-run difference. The forward 32-light overhead from its matched
zero-light scene is 0.512–0.548 ms across the six shadow/visibility
combinations; the 15% initial Forward+ investigation gate still triggers.
A triggered investigation gate does not imply that its candidate wins.

A second A/B used the benchmark's `localized` layout (range 1.75 instead of
8 world units, all other scene inputs unchanged). It ran Direct visibility at
32/64/128 lights, shadows off/on, three repeats per path, another **36 runs and
1,080 measured frames**. On this workload the tile pass avoids most fragment
light evaluations:

| Shadows | Localized lights | Forward build + raster | Tiled build + raster | Tiled change |
| --- | ---: | ---: | ---: | ---: |
| Off | 32 | 0.2348 | 0.1327 | −0.1021 (−43.5%) |
| Off | 64 | 0.4260 | 0.2117 | −0.2143 (−50.3%) |
| Off | 128 | 0.8146 | 0.3716 | −0.4430 (−54.4%) |
| On | 32 | 0.2364 | 0.1340 | −0.1025 (−43.3%) |
| On | 64 | 0.4291 | 0.2133 | −0.2158 (−50.3%) |
| On | 128 | 0.8149 | 0.3723 | −0.4426 (−54.3%) |

The table isolates the changed passes; it does **not** say the whole Player
frame improves by the same percentages. The renderer benchmark also performs
visibility, shadow handling and synchronous image readback. The localized
A/B, like the dense sweep, is a synthetic scene on one device and driver.
Explicit `Tiled` is useful for a measured localized-light scene; `Auto` stays
forward until a representative runtime occupancy predictor and broader scene
set justify changing the default.

## Reproduce and inspect

From a clean build of that revision with the Release benchmark target:

```sh
python3 tools/benchmark_p3_lighting.py --sweep \
  --executable build/linux-release/faset_p3_lighting_benchmark \
  --output /tmp/p3-forward --shadows both --lighting forward \
  --validation off --driver 'NVIDIA GeForce RTX 2080 Ti 595.84'
python3 tools/benchmark_p3_lighting.py --sweep \
  --executable build/linux-release/faset_p3_lighting_benchmark \
  --output /tmp/p3-tiled --shadows both --lighting tiled \
  --validation off --driver 'NVIDIA GeForce RTX 2080 Ti 595.84'
python3 tools/benchmark_p3_lighting.py --compare \
  --forward-summary /tmp/p3-forward/summary.json \
  --tiled-summary /tmp/p3-tiled/summary.json \
  --comparison-output /tmp/p3-comparison.json
```

The checked-in [forward](data/p3-combined-2026-09-24/forward/summary.json)
and [tiled](data/p3-combined-2026-09-24/tiled/summary.json) summaries
contain all acquisition-order and hash metadata. Their adjacent `merged.csv`
files and 216 individual `raw/` CSVs retain every frame. The
[localized paired summary](data/p3-combined-2026-09-24/localized/summary.json)
and its 36 adjacent CSVs retain that fixture. The localized paired recorder saved source/binary hashes but did not embed its
own shader-bundle manifest, so exact historical shader identity for that
secondary fixture is less strongly attested than for the dense comparison.
The localized paired command
varied `--light-layout localized`, `--lights`, `--shadows`, and `--lighting`
while preserving the same binary and 1920 × 1080 output; source and binary
hashes are in its summary. The earlier [Forward+ study](23-p3-forward-plus-2026-09-24.md)
contains a separate untimed tile-occupancy diagnostic and byte-identical
image-parity captures. This repeat is a performance check on combined code,
not a new image-parity or occupancy claim. The timed CSVs deliberately have
`light_tile_counts_valid = 0`; their zero `light_tile_overflow_count` field is
a placeholder, not evidence that any tile avoided overflow. The 128-light
dense fixture is expected from its geometry and tile capacity to fall back
to a full per-fragment scan. Likewise, the shadows-on fixture requests up to
768 local faces at 128 lights but renders only 12 under its fixed atlas budget.
The table does not imply 128 fully shadowed point lights.

Functional correctness belongs to the GPU image tests, including the
Direct/P2 image equivalence, tile overflow fallback, shadows, shader reload,
and temporal cases. The separate [P3 acceptance record](../validation/p3-lighting-2026-09-24/README.md)
tracks those checks and the Linux/Windows CI revisions. The benchmark cannot
prove physical Windows GPU performance, broad scene performance, or that
Vulkan validation was active during its timed captures.
