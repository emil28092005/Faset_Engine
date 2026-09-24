# Final integrated temporal capture at `ed523c6`

This is the P1+P3 integration capture from **clean source revision
`ed523c61c106b8e19614e4d56f0b5d39b8392478`**, after merging the optional
history-pixel diagnostics shader and its window-resize fix, plus the
transactional visibility-switch rollback and async build-signature fixes.
The Linux Debug capture used the physical NVIDIA GeForce RTX 2080 Ti with proprietary driver
595.84. The [provenance manifest](provenance.json) records the exact capture
executable SHA-256 and all 31 loaded SPIR-V/reflection artifact hashes.

The [frame CSV](frames.csv) links **1,386 lossless source PNGs** across Direct,
GPU frustum and GPU occlusion, Off/jittered-current/TAA/upscale-current/Upscale,
2× spatial sources, motion, eight post-open door frames, cut, teleport,
projection/view switch, resize, transparency and UI. Every row reports
`validation_enabled=1`, `validation_errors=0`, and the requested visibility and
temporal paths actually executed. Output was 160×120 except the 319×241 resize
and 320×240 spatial source. The [metrics](metrics.json),
[wire reference sheet](spatial-reference.png) and
[door trail sheet](door-trail-nearest-4x.png) are computed from these images.

All 1,388 PNG files (the 1,386 frames plus two sheets) are byte-identical to
[the earlier branch matrix](../README.md) under the same relative names. This
checks that the integrated diagnostic shader and failure-path fixes, with pixel
counting disabled, did not alter these fixtures. The metrics JSON differs only
in source revision; the frame CSV contains separate timings. In the final matrix, both GPU visibility paths
matched Direct exactly on all 430 comparable ordinary frames per path. The
static wire's frame variation fell 7.2% with TAA and 6.1% with Upscale versus
their same-jitter current-only controls. The 2× box-filter spatial reference
still had lower error than either temporal mode in this fixture; this is no
claim of overall quality superiority. In the opening-door sequence, old red
foreground never returned; the count of edge pixels differing by >8 from the
unobstructed temporal reference went from 121 to 17, then reached zero by the
sixth open frame for TAA, and from 176 to 28, reaching zero by the fifth open
frame for Upscale. Cut, teleport, changed projection/view and resize matched
current-only immediately in the fixed reset ROI.

The separate [720p CSV](profile-720p-debug.csv) has 30 measured frames per Off,
TAA and 0.67 Upscale after ten warm-up frames for each mode, with rotating mode
order. Every one of its 90 rows reports active validation and zero errors.
It is a sparse one-cube/one-wire Direct fixture with full-frame synchronous
capture in Debug, not a quiet Release gameplay benchmark. p95 below uses
linear interpolation at rank `(n − 1) × 0.95`:

| Mode | Full GPU p50 / p95 | Resolve p50 / p95 | Renderer CPU p50 / p95 | Explicit Vulkan allocation |
| --- | ---: | ---: | ---: | ---: |
| Off | 0.558 / 0.581 ms | — | 2.109 / 2.437 ms | 43.02 MiB |
| TAA | 0.676 / 0.697 ms | 0.084 / 0.101 ms | 2.472 / 2.770 ms | 76.77 MiB |
| Upscale 0.67 | 0.675 / 0.702 ms | 0.083 / 0.101 ms | 2.475 / 2.836 ms | 68.52 MiB |

The diagnostic counter toggle was off, so no per-pixel counter readback was
included in these timings. The 0.67 mode did not improve the whole GPU time on
this sparse fixture. The [parent P3 record](../../README.md) explains the
quality limitations and the earlier, separately timed 720p checkpoint.

To reproduce from this source revision, install the
[pinned analyzer dependencies](../../../../../tools/requirements-temporal-quality.txt)
into a Python environment, configure the Linux Debug build and run:

```sh
cmake --build build/linux-debug --target faset_render_temporal_acceptance_tests --parallel 2
ctest --test-dir build/linux-debug --no-tests=error -R '^render_temporal_quality_matrix$' --output-on-failure
build/linux-debug/faset_render_temporal_acceptance_tests --capture-quality /tmp/faset-p3-final-new
build/linux-debug/faset_render_temporal_acceptance_tests --profile-720p /tmp/faset-p3-final-new-profile.csv
python3 tools/analyze_temporal_quality_matrix.py --input /tmp/faset-p3-final-new \
  --output /tmp/faset-p3-final-new-analyzed \
  --revision ed523c61c106b8e19614e4d56f0b5d39b8392478 \
  --driver 'NVIDIA proprietary 595.84'
```

Use fresh paths: the analyzer rejects an existing output directory. The
separate [resize diagnostics regression](https://github.com/emil28092005/Faset_Engine/blob/main/tests/render_temporal_diagnostics_tests.cpp)
exercises counter readback during swapchain recreation on a physical GPU; this
quality capture leaves that optional diagnostic path disabled.

The [visibility-switch rollback test](https://github.com/emil28092005/Faset_Engine/blob/main/tests/render_reload_tests.cpp)
checks that a failed GPU-mode transition preserves live TAA output and history
and that the transition can be retried after restoring the shader bundle. The
[P1 queued-source regression](https://github.com/emil28092005/Faset_Engine/blob/main/tests/editor_schema_signature_tests.cpp)
checks that Build and Export report the worker snapshot hash rather than the
enqueue-time hash. These failure-path fixes did not change any matrix image.
