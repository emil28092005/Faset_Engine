# Build from source

!!! note "Verified build profiles"
    The Editor, Player and both sample exports passed the recorded Linux and Windows
    profiles. Linux rendering used an RTX 2080 Ti; Windows CI used SwiftShader. This
    does not certify every graphics driver or display configuration.

## Linux prerequisites

The selected toolchain is C++20, CMake 3.25 or later, Ninja, Clang, and Python 3.12+.
Graphical builds need Vulkan 1.3 headers/loader and a compatible driver.
SDL3, FreeType and HarfBuzz are built from pinned source archives.

On Ubuntu, install the native build tools before configuring:

```sh
sudo apt install clang ninja-build cmake python3 python3-venv pkg-config \
  libvulkan-dev vulkan-validationlayers libx11-dev libxext-dev libxrandr-dev \
  libxcursor-dev libxi-dev libxfixes-dev libxkbcommon-dev libwayland-dev \
  xvfb
```

`xvfb` is used for automated window tests. A normal desktop session does not need it.

## Configure, build, test

```sh
python3 tools/fetch_slang.py
cmake --preset linux-debug
cmake --build --preset linux-debug --parallel
ctest --preset linux-debug
```

Create a project and open the native Editor:

```sh
build/linux-debug/faset_editor --project "$PWD/MyGame" --new MyGame --dimension 3
```

You can also run `build/linux-debug/faset_editor` without arguments to open the
project launcher and create or select a project using the native interface.

Use **Build** after changing `MyGame/Scripts/Gameplay.cpp`, then **Play**.
The Player runs separately. Stop it before changing and rebuilding C++ gameplay.
See [MCP and CLI](../editor/mcp.md) for headless authoring and automation.

For an optimized build use `linux-release`. The `linux-sanitize` preset enables
AddressSanitizer and UndefinedBehaviorSanitizer for tests without the graphics backend.

## Optional Lua module

Engine development builds enable `FASET_ENABLE_LUA` by default. Lua 5.4.9 is compiled
from its checksum-pinned source archive; no system Lua installation is required.
Pass `-DFASET_ENABLE_LUA=OFF` to omit the VM and bindings. The Editor's project
build/export service selects this flag from `scripting.lua.scripts` in
`project.faset.json`, so C++-only games do not link Lua.

See the [Lua guide](../scripting/lua.md) for the manifest, a Lua-only project,
hot reload, and external-editor/LuaLS setup. Headless CPU checks can be run with:

```sh
cmake --preset linux-debug -DFASET_BUILD_RENDERER=OFF -DFASET_BUILD_EDITOR=OFF
cmake --build --preset linux-debug --parallel
ctest --preset linux-debug
```

These checks do not verify the graphical Player or renderer.

## Dependencies and offline builds

Dependency source URLs, commits, and archive SHA-256 values are stored in
`dependencies.lock.json`. CMake downloads them on the first configuration.
To prefetch them for later offline use:

```sh
python3 tools/fetch_dependencies.py
python3 tools/fetch_dependencies.py --verify-only
```

Cached archives live in `.cache/downloads` and are not committed. Local compilers,
system development libraries, and the Slang compiler must also be available before
disconnecting. Prefetching source archives alone is not a complete offline SDK.

## Windows prerequisites

Use an x64 Visual Studio Developer shell with the Windows SDK, MSVC runtime libraries,
LLVM `clang-cl`, Ninja, CMake 3.25+, Python 3.12+, and the Vulkan SDK available.
The commands below use the Python `py` launcher; substitute `python` if your
installation exposes that command instead. Use the `windows-debug` or
`windows-release` presets.

Enable **Win32 long paths** on the Windows development machine before starting the
build shell. Faset's executable manifest declares long-path support, and its direct
file IO uses wide extended paths; external CMake/Ninja/compiler tools also need a
compatible host policy for deeply nested build/cache directories. An administrator
can enable the policy once in PowerShell:

```powershell
New-ItemProperty -Path 'HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem' `
  -Name LongPathsEnabled -Value 1 -PropertyType DWORD -Force
```

Open a new build shell afterwards; Windows may require a restart for existing
processes. See Microsoft's [long-path requirements](https://learn.microsoft.com/en-us/windows/win32/fileio/maximum-file-path-limitation).
Windows CI enables and records this developer profile explicitly.

```powershell
py tools/fetch_slang.py
cmake --preset windows-debug
cmake --build --preset windows-debug --parallel
ctest --preset windows-debug
```

Windows acceptance uses a fresh native CI checkout, full Editor build, launcher
Create/Open tests, native window/MCP tests and standalone Release exports. Its
software Vulkan driver is a CI fixture; install your normal hardware Vulkan driver
on a development desktop. See the
[acceptance dossier](https://github.com/emil28092005/Faset_Engine/blob/main/docs/validation/mvp-acceptance.md)
for observed results.

Native Wayland programmatic restore was skipped when the tested compositor declined
the operation; XWayland passed. If this affects your desktop, run the Editor with
`SDL_VIDEODRIVER=x11`. Real system IME composition and movement between physical
monitors with different scale factors remain compatibility checks, beyond the
passing deterministic text/DPI tests.

The repository's `docs/TOOLCHAINS.md` records the exact compiler, SDK and GPU profiles
used in observed validation, separately from the minimum tool requirements above.
