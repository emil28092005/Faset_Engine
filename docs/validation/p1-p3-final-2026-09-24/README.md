# P1 and P3 integrated acceptance — 2026-09-24

The final code revision is `ed523c61c106b8e19614e4d56f0b5d39b8392478`.
The [Release export report](report.json) records a clean source checkout and the
exact Editor SHA-256. The Editor exported `collect-2d`, `collect-3d` with its
Blender exit arch, and the Lua-only example. Each package was relocated to a
Unicode-named path outside its disposable source project; those source paths
were hidden while the Player validated and rendered from an unrelated working
directory. All three packages passed **120/120** offscreen frames on a physical
NVIDIA GeForce RTX 2080 Ti with active Vulkan validation and zero errors. The
[file checksums](SHA256SUMS.txt), package manifests for
[2D](evidence/collect-2d-manifest.json),
[3D](evidence/collect-3d-manifest.json) and
[Lua](evidence/lua-manifest.json), and raw [2D](evidence/collect-2d-profile.json),
[3D](evidence/collect-3d-profile.json), and [Lua](evidence/lua-profile.json)
profiles preserve the result without duplicating native build directories.

| Final-code check | Result |
| --- | --- |
| Linux Debug full CTest | 76 passed, 1 compositor-dependent native window skip, 0 failed (77 registered) |
| Linux Release P1/P3-focused CTest | 24 passed, 0 failed |
| Relocated Release games on physical Linux GPU | C++ 2D, C++ 3D, Lua-only: 120 frames each; validation active, 0 errors |
| [Native/manual CI](https://github.com/emil28092005/Faset_Engine/actions/runs/35949830254) | Linux and Windows native checks plus strict MkDocs passed |
| [Windows software-Vulkan CI](windows-swiftshader/README.md) | 78/78 CTests, Release export integration and three relocated 120-frame C++/Lua games passed on SwiftShader; validation layer unavailable |

The P1 queued-edit regression verifies that an asynchronous Build or Export
reports the gameplay-source hash captured by its worker, not the hash from when
the command entered the queue. The P3 shader-reload regression first reproduced
a null-pipeline crash during Direct-to-GPU switching with TAA active; the
failed transition now preserves the old pipelines and history and can be
retried after restoring the shader package. Both regressions are in the full
Linux suite and Windows native/graphics checks.

The [final P3 temporal matrix](../p3-temporal-2026-09-24/matrix/integrated-ed523c6/README.md)
records 1,386 lossless frames across Direct, GPU frustum and GPU occlusion,
plus an independent 90-frame 720p profile. All captures used active Vulkan
validation with zero errors; its 1,388 PNG files are byte-identical to the
earlier branch matrix. The [integrated lighting study](../../studies/24-p3-integrated-forward-plus-2026-09-24.md)
contains 7,560 frame rows from the earlier clean `4a3453e` revision, on the
same physical GPU and shader path. Its dense tile build+raster case was slower
than forward; the localized-light case was faster, so `Auto` remains forward.
The P1 [240-frame Release reference](../p1-iteration-2026-09-24/final-release/README.md)
passed the original small-demo timing budgets but missed the old 20 MiB
explicit-allocation target at 43.02/43.12 MiB because both shadow atlases are
allocated eagerly. The final revision does not change that allocation policy.

TAA and 0.67 Upscale remain opt-in. The 2× spatial reference was more accurate
on the fixed thin-wire fixture, and Upscale did not reduce whole-frame GPU time
in the sparse 720p test. These checks cover one physical Linux GPU and Windows
SwiftShader functionality; a physical Windows GPU and broader driver families
still need separate testing.

Reproduce the relocated Release check from a clean source checkout with the
documented dependencies:

```sh
cmake --preset linux-release
cmake --build --preset linux-release --target faset_editor --parallel 2
python3 tools/verify_playable_exports.py \
  --editor build/linux-release/faset_editor \
  --output /tmp/faset-p1-p3-export-new \
  --standalone-root /tmp/faset-p1-p3-games-new \
  --include-lua
```

Use fresh output paths. The verifier retains the full packages and captures
outside the repository, while this dossier commits the report, selected
manifests, per-frame profiles, and run records.
