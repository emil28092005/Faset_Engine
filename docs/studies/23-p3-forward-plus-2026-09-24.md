# P3 Forward+ experiment: correctness and cost on localized lights

The fixed 1920×1080 P3 benchmark crossed the agreed threshold for trying
Forward+. A depth-free 16×16 tiled implementation now exists, but the measured
**build + raster** cost is higher than a full light scan on that benchmark's
dense lights. `RendererConfig::lighting_mode = Auto` therefore keeps the forward
path. `Tiled` is an explicit option for scenes whose projected light volumes
are localized. There is no unmeasured automatic occupancy heuristic.

This is a follow-up to the fixed-scene baseline sweep, which is being merged
as a separate study. It compares both paths in the same source revision
`a0a4e29d480ed3344f19bd3565d48668ca913fed`. The baseline's dense
placement remains the default. An explicit `--light-layout localized` changes
only point-light range from 8 to 1.75 world units; camera, nine casters,
receiver, positions, colors, light count, and output size are unchanged. The
localized fixture is a separate workload, not a replacement for the fixed
baseline gate.

## Renderer behavior and safety

The compute pass builds up to 64 stable-order light indices per screen tile.
It tests each world-space range sphere against four clip-space tile planes.
It does not use depth or reject near-plane intersections. A tile with more than
64 candidates sets an overflow bit; the fragment shader then scans **all**
submitted lights for that tile. Zero lights, missing capability, excessive
buffer size, failed optional allocation, and `Auto` use the forward path. The
shader contract checks the new compute entry's descriptors and 96-byte push
constants; Direct and P2 GPU graphics still use materials at set 0, lighting
at set 1, and GPU scene data at set 2. The tile list is set 1 binding 4 in the
shared fragment shader. Sprite/UI shading returns before tile reads.

The Linux Vulkan image test compares forward and tiled output in Direct, GPU
frustum, and GPU occlusion modes, including a cropped scene viewport, near-plane
crossing point light and shadow, resize, an offscreen light, and 80 coincident
lights that exceed tile capacity. Every overflowing tile falls back to the full
list. Shader reload preserves a working tiled pipeline after invalid bytecode
and recreates it after a valid reload. A separate 1920×1080 capture with 128
localized lights was byte-identical across both paths; its SHA-256 is in the
[provenance record](data/p3-forward-plus-provenance-2026-09-24.json).

## Measurement

The device was NVIDIA GeForce RTX 2080 Ti with NVIDIA driver 595.84.0.0,
Linux Clang Release, Direct visibility, shadows off, 1920×1080. Each mode had
three independent process runs with ten warm-up and thirty measured frames.
Forward/tiled run order alternated. The table uses the median of the three
per-run medians in milliseconds. The tile build column is an actual GPU
timestamp; `build + raster` also includes post raster if present. The dense
and localized CSVs contain every one of the 1080 measured frames, with a
`source_csv` identifier. The executable and all loaded `.spv`/reflection
SHA-256 values are in the provenance record.

| Light layout | Lights | Forward raster | Tile build | Tiled raster | Tiled build + raster | Tiled change |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Dense fixed scene | 32 | 0.5500 | 0.0617 | 0.5527 | 0.6144 | +0.0644 ms (11.7% slower) |
| Dense fixed scene | 64 | 1.0701 | 0.1177 | 1.0740 | 1.1921 | +0.1220 ms (11.4% slower) |
| Dense fixed scene | 128 | 2.1172 | 0.2219 | 2.1164 | 2.3388 | +0.2216 ms (10.5% slower) |
| Localized range 1.75 | 32 | 0.2336 | 0.0555 | 0.0758 | 0.1312 | −0.1025 ms (43.9% faster) |
| Localized range 1.75 | 64 | 0.4254 | 0.1060 | 0.1057 | 0.2109 | −0.2144 ms (50.4% faster) |
| Localized range 1.75 | 128 | 0.8094 | 0.2048 | 0.1643 | 0.3691 | −0.4404 ms (54.4% faster) |

At 32 dense lights, the first forward process had a 0.7405 ms run median;
the other two were 0.5488 and 0.5500 ms. A single paired run would have
incorrectly suggested a tiled win. The median of three process medians and a
separate earlier repeat both support the slower dense result. This is why
`Auto` remains forward despite the localized-scene gain. The total GPU frame
also includes visibility, shadow fallback, copies, and synchronous readback;
the table isolates the passes that the optimization changes. For example, at
32 localized lights the full GPU frame was 1.6494 ms forward and 1.6472 ms
tiled, essentially unchanged despite lower build + raster cost. At 128 it
was 2.2788 versus 1.7948 ms.

One diagnostic frame per layout/count copied the tile buffer after the timed
draw. That copy was **not enabled** in the 1080 performance frames. The grid
has 8160 tiles and a 64-index capacity per tile.

| Layout | Lights | Stored candidates across tiles | Overflowed tiles |
| --- | ---: | ---: | ---: |
| Dense | 32 | 259,896 | 0 |
| Dense | 64 | 519,792 | 0 |
| Dense | 128 | 522,240 | 8,160 |
| Localized | 32 | 38,237 | 0 |
| Localized | 64 | 76,103 | 0 |
| Localized | 128 | 152,202 | 0 |

The dense 128 candidate count is capped at 64 × 8160 stored slots; all tiles
overflow and correctly evaluate all 128 lights in the fragment shader. This
explains why paying for tile construction cannot help that frame. The localized
128 scene averages about 19 stored candidates per tile and avoids fallback.

Raw data: [all paired frames](data/p3-forward-plus-ab-2026-09-24.csv),
[diagnostic frames](data/p3-forward-plus-diagnostics-2026-09-24.csv), and
[binary/shader provenance](data/p3-forward-plus-provenance-2026-09-24.json).

## Verification and scope

At the implementation revision, Linux Debug built all targets and passed
62/63 CTests, with the compositor-dependent window lifecycle case skipped and
no failures. The pinned Linux SwiftShader ICD passed all six P3 cases, including
the tiled parity/overflow test. The [lighting validation record](../validation/p3-lighting-2026-09-24/README.md)
retains those logs. These are functional checks on Linux and software Vulkan,
not physical Windows GPU performance. The A/B numbers apply to one GPU, driver,
camera, receiver and two synthetic light layouts. They do not establish an
engine-wide speedup. A measured runtime occupancy predictor and representative
game scenes are prerequisites before changing `Auto` from forward.
