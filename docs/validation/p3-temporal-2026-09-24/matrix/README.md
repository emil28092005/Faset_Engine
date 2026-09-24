# P3 temporal image-quality matrix: branch checkpoint

This is a functional and image-quality capture from
`fe3a589174f8fe8e35ee231fe74fe938f4dd3cbd` on
`feat/p3-temporal-integration`, before the later pixel-diagnostics shader was
merged into the combined P1+P3 branch. It is separate from the earlier Direct
capture at `1a1f69f` and from the combined lighting performance sweep at
`4a3453e`. Repeating this matrix on the final combined source is necessary
before claiming its exact images as integrated release evidence.

The run used Linux Debug, NVIDIA GeForce RTX 2080 Ti, proprietary driver
595.84, 160×120 output except the 319×241 resize scene and 320×240 spatial
source. The [raw frame CSV](branch-fe3a589/frames.csv) contains 1,386 linked
lossless PNGs, requested and effective visibility, actual Vulkan validation
activation, per-frame reset/history fields, extents and timings. The
[metric JSON](branch-fe3a589/metrics.json) contains fixed ROIs and per-frame
measurements. Vulkan validation was **enabled** on all frames and reported zero
errors. Both GPU paths remained effective; neither silently fell back to
Direct. The capture is a small deterministic scene test, not a real-game
benchmark.

## Matrix and references

For each Direct, GPU-frustum and GPU-occlusion path, the same sequences were
captured with Off, jittered current-only TAA, accumulated TAA, current-only
Upscale at 0.67 and accumulated Upscale at 0.67. Current-only uses the same
jitter/internal extent but cuts history every frame. Sequences are a 16-phase
thin static wire, 16-phase slow pan, 16-frame moving cube, 16-frame
unobstructed door background, door closed for two frames then open for eight,
and two-frame cut, teleport, changed projection, changed view ID, resize and
transparency/sprite/UI cases. Every visibility path also has a 320×240 Off
source for wire and pan, downsampled to 160×120 by a 2×2 box in
display-encoded RGB. That four-sample spatial result is a bounded reference,
not ground truth or an HDR comparison.

The [wire/pan comparison](branch-fe3a589/spatial-reference.png) shows the
Direct phase-15 outputs and the downsampled reference. The
[door trail sheet](branch-fe3a589/door-trail-nearest-4x.png) enlarges the same
30×35 Direct crop fourfold with nearest-neighbor sampling over all eight
post-open frames. The other P2 images are linked individually by the raw CSV.

| 160×120 Direct wire ROI | Off | Current-only | TAA | Upscale current-only | Upscale |
| --- | ---: | ---: | ---: | ---: | ---: |
| Mean frame-to-frame RGB delta, phases 5–15 | 0.000 | 0.388 | 0.360 | 0.439 | 0.412 |
| Mean RGB error to same-phase 2× box reference, phases 4–15 | 0.257 | 0.296 | 0.270 | 0.368 | 0.341 |

TAA and Upscale reduce shimmer relative to their own jittered current-only
controls by 7.2% and 6.1% in this wire ROI. The unjittered Off result is
closer to this particular 2× spatial reference than TAA, and full-resolution
outputs are closer than the 0.67 Upscale outputs. During the pan, TAA's
same-phase reference error is 0.298 versus Off's 0.241; temporal accumulation
barely changes pan frame delta. The fixture therefore supports a narrow
stationary-edge benefit, not a general image-quality ranking.

For the open door, pixels in the 30×35 edge ROI whose channel error exceeds 8
against the same-phase unobstructed temporal background count:

| Open frame | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| TAA | 121 | 17 | 3 | 3 | 3 | 0 | 0 | 0 |
| Upscale | 176 | 28 | 28 | 28 | 0 | 0 | 0 | 0 |

The old-door ROI had zero red foreground pixels across all eight open frames.
The newly exposed center matches current-only immediately. Both paths met the
fixed edge gate (at most 32 pixels above 8 and mean RGB error at most 0.75)
from open frame 1 onward, though enlarged Upscale images still show a
phase-dependent one-pixel dark edge on some later phases. This threshold is a
bounded trail measure, not proof of visually perfect convergence. Cut,
unmarked teleport, changed projection, changed view ID and resize phase 1
matched current-only exactly in their fixed reset ROI for TAA and Upscale.

Across all 430 ordinary frames per P2 path (the five modes and eleven
sequences, excluding 2× sources), GPU-frustum and GPU-occlusion captures were
pixel-identical to Direct on this GPU: mean and maximum channel differences
were zero. That establishes parity for these small scenes, not for arbitrary
geometry or platforms.

## Reproduce without overwriting committed evidence

From this source revision and a configured Linux Debug build:

```sh
cmake --build build/linux-debug --target faset_render_temporal_acceptance_tests --parallel 2
ctest --test-dir build/linux-debug --no-tests=error -R '^render_temporal_quality_matrix$' --output-on-failure
build/linux-debug/faset_render_temporal_acceptance_tests --list-quality-runs
build/linux-debug/faset_render_temporal_acceptance_tests \
  --capture-quality /tmp/faset-p3-matrix-fe3a589
python3 -m venv .cache/temporal-quality-venv
.cache/temporal-quality-venv/bin/python -m pip install \
  -r tools/requirements-temporal-quality.txt
.cache/temporal-quality-venv/bin/python tools/analyze_temporal_quality_matrix.py \
  --input /tmp/faset-p3-matrix-fe3a589 \
  --output /tmp/faset-p3-matrix-report-fe3a589 \
  --revision fe3a589174f8fe8e35ee231fe74fe938f4dd3cbd \
  --driver 'NVIDIA proprietary 595.84'
```

The analyzer refuses an existing output directory so old evidence cannot be
silently overwritten. Choose fresh input and output directories for each
revision. The 720p cost profile remains in the parent validation record; the
12-second matrix run was for correctness and captures, not quiet profiling.

A separate [90-row 720p Debug profile](profile-720p-branch-5cb818f.csv) was
captured from `5cb818f0cc6e6b1a435515deb7dfd7001df4d2c7`, whose profile
CSV adds the actual `validation_enabled` field. It recorded `1` and zero
errors on every measured frame. Its timings were not taken in a reserved quiet
window, so use the parent record's methodology and a final integrated rerun
for performance decisions. Reproduce this CSV from that code revision with:

```sh
build/linux-debug/faset_render_temporal_acceptance_tests \
  --profile-720p /tmp/faset-p3-720p-5cb818f.csv
```
