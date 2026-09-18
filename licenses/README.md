# Third-party notices

Faset source dependencies are pinned in `dependencies.lock.json`. These notices apply to the named third-party components; they do not assign a license to Faset itself.

- **sdl3** (Zlib): [96292a5b4642](https://github.com/libsdl-org/SDL/tree/96292a5b464258a2b926e0a3d72f8b98c2a81aa6), notice in `sdl3.txt`.
- **entt** (MIT): [d4014c74dc37](https://github.com/skypjack/entt/tree/d4014c74dc3793aba95ae354d6e23a026c2796db), notice in `entt.txt`.
- **box2d** (MIT): [8c661469c950](https://github.com/erincatto/box2d/tree/8c661469c9507d3ad6fbd2fea3f1aa71669c2fe3), notice in `box2d.txt`.
- **box3d** (MIT): [f555ee42084e](https://github.com/erincatto/box3d/tree/f555ee42084e0b43cbffa863f40bff8117c08896), notice in `box3d.txt`.
- **imgui** (MIT): [5d4126876bc1](https://github.com/ocornut/imgui/tree/5d4126876bc10396d4c6511853ff10964414c776), notice in `imgui.txt`.
- **cgltf** (MIT): [360db1a95480](https://github.com/jkuhlmann/cgltf/tree/360db1a95480fe102ae9c69b27c5d101167ff5ba), notice in `cgltf.txt`.
- **stb** (MIT OR Unlicense): [2c980bb59875](https://github.com/nothings/stb/tree/2c980bb59875b0d32144a71867fbdebb2f77cd20), notice in `stb.txt`.
- **nlohmann/json** (MIT): pinned in the dependency lock, notice in `json.txt`.

- **FreeType** (FreeType License): pinned 2.13.3, notice in `freetype.txt`. Portions of this software are copyright © The FreeType Project (www.freetype.org). All rights reserved.
- **HarfBuzz** (MIT-style): pinned 10.4.0, copyright and permissions in `harfbuzz.txt`.
- **Noto Sans** (SIL Open Font License 1.1): editor font only; see `../assets/fonts/OFL.txt` and `../assets/fonts/NOTICE.md`.

The default Editor build uses the pinned FreeType/HarfBuzz archives. An explicit
`FASET_USE_SYSTEM_TEXT_LIBRARIES=ON` uses installed versions; distributors must retain
the notices appropriate to those installations. These font libraries and the editor
font are not linked into or required by exported games.
