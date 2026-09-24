# Temporal rendering

Faset renders the scene with **Off** by default. In the optional Editor diagnostics
panel (**F12**), choose **TAA** to accumulate a full-resolution scene over successive
frames, or **Upscale** to render the scene at a lower resolution and reconstruct it
at the output resolution. The Upscale slider accepts 50–99%; 67% is a useful
starting point for visual comparison. UI text and controls always render at output
resolution after the scene resolve. Shadow maps keep their own unjittered views.

The diagnostic selector affects only the current Editor viewport. It does not edit
the scene, gameplay code, or an exported Player. The panel's **Requested** and
**Effective** fields identify a device fallback. It also shows internal and output
extent, whether the previous completed frame's color history was eligible, the
reason it reset, and separate GPU times for resolve, composite and UI where
timestamp queries are available. A reset on the first frame, camera cut, changed
view, resize, scale switch or compatible shader reload is expected. A valid history
does not imply every pixel reused it: newly visible surfaces can still reject
their individual history samples.

For a Player or exported game, select the mode at launch:

```sh
./faset_player --headless --frames 120 --temporal taa --profile taa.json
./faset_player --headless --frames 120 --temporal upscale \
  --render-scale 0.67 --profile upscale.json
```

`--temporal` accepts `off`, `taa`, or `upscale`. Off and TAA use scale `1`; Upscale
requires a scale from `0.5` inclusive to `1` exclusive. An invalid mode or scale
stops startup with an error. If Vulkan compute or the required image formats are
unavailable, the renderer falls back to Off and records its effective mode and
reason in the profile. Direct, GPU frustum and GPU occlusion visibility can be
combined with either temporal mode. See [Profiling](profiling.md) for how to compare
their timings fairly.

Native renderer users can make the same choice without modifying gameplay scripts:

```cpp
faset::render::RendererConfig config;
config.temporal_mode = faset::render::TemporalMode::Upscale;
config.render_scale = 0.67f;
faset::render::Renderer renderer(config);

// A live viewport switch recreates scene targets and resets color history.
renderer.set_temporal_mode(faset::render::TemporalMode::TAA);
```

Provide a stable `DrawItem::instance_key` for moving opaque objects so the renderer
can find their previous model transform. Camera cuts must be marked in the
`Snapshot`; cuts, teleports and incompatible projection changes reject old history.
World transparency and sprites use the scene depth/order and reject stale color on
their reactive pixels. TAA and Upscale are optional image-quality paths; compare
them against Off on the actual game scene, especially thin geometry, slow pans,
newly revealed surfaces and moving transparent content.

These are first-generation, opt-in reconstruction modes. They can reduce shimmer
on a stationary edge while lowering the peak brightness of a subpixel line, and
a newly revealed edge can differ by one pixel from its settled appearance. The
amount depends on scene content, resolution and motion. Upscale also trades
internal render resolution for resolve cost and extra images; it is not always
faster. Compare Off, TAA and Upscale at the target output resolution with both
still and moving cameras, and inspect thin objects and opening doors before
choosing a mode for a game. The [P3 temporal validation record](https://github.com/emil28092005/Faset_Engine/blob/main/docs/validation/p3-temporal-2026-09-24/README.md)
contains source captures, paired image measurements and a bounded 720p cost
profile.
