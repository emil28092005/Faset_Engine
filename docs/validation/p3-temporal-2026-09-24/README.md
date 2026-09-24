# P3 temporal reconstruction: image quality and cost, 2026-09-24

This record covers Faset's first-generation, opt-in TAA and temporal upscaler.
The 370-frame Direct captures and 720p cost profile below were executed from
`1a1f69f7e88b3ea69986931dfcf6baf9b43a431c` on
`feat/p3-temporal-integration`. The later [three-path quality matrix](matrix/README.md)
was executed from `fe3a589174f8fe8e35ee231fe74fe938f4dd3cbd` before the
pixel-diagnostics shader merge. The combined P1+P3 performance sweep at
`4a3453e` is a different run, not the source of these images. The physical
Vulkan device was an NVIDIA GeForce RTX 2080 Ti with proprietary driver 595.84
on Linux. Both original Direct fixtures requested Vulkan validation and every
recorded frame reported zero
validation errors. These observations do not establish physical Windows GPU
behavior or high-end reconstruction quality.

The [final integrated matrix](matrix/integrated-ed523c6/README.md) separately
repeats all 1,386 frames at clean `ed523c6`, after the optional diagnostics
shader, resize fix, and visibility-switch rollback. Actual Vulkan validation
was active with zero errors;
all captured PNGs are byte-identical to the earlier branch matrix. Its
independent 720p profile is recorded with exact binary and shader hashes.

## Raw artifacts and method

- [Lossless PNG example](captures/wire-static-taa-15.png) and
  [frame-level CSV linking every capture](frames.csv) cover
  the Direct visibility path at 160×120, except the 319×241 resize frame.
  The sequences are static subpixel wire (16 phases), slow camera pan (16),
  moving cube (16), unobstructed background (16), opening door (4), camera cut
  (2), resize (2), and translucent world geometry plus sharp UI (2). Each has
  Off, jittered current-only TAA, accumulated TAA, current-only Upscale, and
  accumulated Upscale captures. The current-only controls mark each frame as a
  camera cut: they retain the same 16-phase jitter and internal extent while
  rejecting history. Upscale uses render scale 0.67. Off has no jitter.
- [Computed metrics](metrics.json) contain every sample, fixed ROI coordinates,
  steady-state timing distributions and paired image errors. The
  [contact sheet](contact-sheet.png) shows representative source frames without
  artistic retouching. The [door edge detail](door-edge-nearest-5x.png) crops
  phase 3 and enlarges pixels 5× with nearest-neighbor sampling; its second row
  is the same-jitter unobstructed reference.
  `tools/analyze_temporal_quality.py` converts the raw PPM output of
  `faset_render_temporal_acceptance_tests --capture-quality DIR` to PNG
  losslessly and computes the metrics. It needs the pinned NumPy and Pillow
  versions in `tools/requirements-temporal-quality.txt`.
- [720p raw profile](profile-720p-debug.csv) contains 30 measured frames per
  Off/TAA/Upscale mode after 10 warm-up frames, with rotating mode order.
  `faset_render_temporal_acceptance_tests --profile-720p CSV` reproduces this
  fixed Direct scene: one opaque cube and one thin wire, output 1280×720,
  Upscale internal 858×483. It was a Linux Debug run with validation requested,
  not a Release or gameplay frame-rate result. GPU timestamps include submitted
  work and the synchronous image-to-buffer capture; renderer CPU time includes
  the wait and host readback.

Reproduction from a configured build:

```sh
cmake --build build/linux-debug --target faset_render_temporal_acceptance_tests --parallel 2
ctest --test-dir build/linux-debug --no-tests=error -R '^render_temporal_acceptance$' --output-on-failure
build/linux-debug/faset_render_temporal_acceptance_tests --capture-quality /tmp/faset-p3-quality
python3 tools/analyze_temporal_quality.py --input /tmp/faset-p3-quality \
  --output docs/validation/p3-temporal-2026-09-24 \
  --revision 1a1f69f7e88b3ea69986931dfcf6baf9b43a431c \
  --driver 'NVIDIA proprietary 595.84'
build/linux-debug/faset_render_temporal_acceptance_tests --profile-720p /tmp/faset-p3-720p.csv
```

## Image-quality observations

The static variation measure is the mean absolute RGB difference between
consecutive frames over phases 5–15 in the fixed ROI; lower means less frame
shimmer, but it says nothing by itself about retained contrast. Wire energy is
the mean summed RGB value over phases 4–15 in that ROI, and peak is the mean
brightest channel per frame. All color figures use 8-bit output values.

| 160×120 fixture | Current-only | Accumulated | Change |
| --- | ---: | ---: | ---: |
| Static wire RGB frame delta, TAA | 0.388 | 0.360 | −7.2% |
| Static wire RGB frame delta, Upscale | 0.439 | 0.412 | −6.1% |
| Static wire ROI RGB energy, TAA | 8,234 | 8,275 | +0.5% |
| Static wire ROI RGB energy, Upscale | 7,429 | 7,485 | +0.8% |
| Static wire mean peak, TAA | 179 | 167 | −6.7% |
| Static wire mean peak, Upscale | 179 | 161 | −9.9% |
| Slow pan RGB frame delta, TAA | 0.399 | 0.398 | −0.2% |
| Moving cube RGB frame delta, TAA | 1.930 | 1.917 | includes real motion |

The separate static cube-edge acceptance ROI fell from 0.832 jittered
current-only to about 0.739 under TAA. Its exact percentage does not transfer
to the thin wire. The original resolver achieved a much larger apparent wire
variance reduction by dimming the wire about 15%; the final depth-aware
resolver retains total wire energy within 1% in this fixture. It still reduces
peak contrast and offers little gain during a slow pan. Upscale is not
equivalent to a full-resolution reference for thin detail.
Off has zero static frame variation because it has no jitter; that does not
make its edges alias-free.

The door's newly exposed center matched current-only within one RGB value on
the first open frame. No red pixels from the old door appeared in the tested
old-door ROI. Against a phase-aligned, unobstructed temporal background, the
30×35 edge ROI had 121 TAA and 176 Upscale pixels with any channel difference
above 8 on the first open frame, then 17 and 28 respectively on the next frame.
The enlarged comparison shows a one-pixel dark green/top-bottom edge
difference, with no displaced red silhouette. It is a bounded residual, not a
claim of zero halo. The explicit cut output matched
current-only exactly in its ROI; resize and UI pixels matched their controls.
The existing Direct, GPU-frustum and GPU-occlusion tests separately cover
moving reveal, cut, reset, resize, shader reload and Vulkan validation.

## 1280×720 cost on this sparse scene

All values below are milliseconds except allocation. p95 is linearly
interpolated at rank `(n − 1) × 0.95` among the 30 measured frames.

| Mode | Full GPU p50 / p95 | Resolve p50 / p95 | Composite p50 / p95 | Renderer CPU p50 / p95 | Live Vulkan allocation |
| --- | ---: | ---: | ---: | ---: | ---: |
| Off | 0.560 / 0.580 | — | — | 2.256 / 2.527 | 43.02 MiB |
| TAA | 0.671 / 0.715 | 0.081 / 0.101 | 0.026 / 0.028 | 2.545 / 2.927 | 76.77 MiB (+33.75) |
| Upscale 0.67 | 0.672 / 0.703 | 0.079 / 0.099 | 0.025 / 0.027 | 2.630 / 3.448 | 68.52 MiB (+25.50) |

Main raster p50 was only 0.015–0.018 ms because this fixture is sparse; at
this resolution and content, lower scene resolution did not pay for the output
resolve/composite. The depth-aware 2×2 history sampling is included in these
costs. A dense game scene, Release build, another driver or display path may
have different results. The renderer's synchronous full-frame capture makes
these CPU and GPU totals unsuitable as unqualified gameplay FPS predictions.

TAA and Upscale therefore remain opt-in. Compare Off and a jittered
current-only capture at the game's target resolution, watch thin-object peak
contrast and newly revealed edges, and profile the whole frame. These fixtures
are a bounded regression check, not parity with Unreal TSR, FSR or DLSS.
