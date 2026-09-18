# Build from source

!!! warning "Foundation checkpoint"
    These instructions initially cover the build foundation. The integrated editor,
    sample projects, and packaging steps are being added and verified during MVP implementation.

## Linux prerequisites

The selected toolchain is C++20, CMake 3.25 or later, Ninja, and Clang.
Graphical builds need Vulkan 1.3 headers/loader and a compatible driver.
SDL3 is built from a pinned source archive.

On Ubuntu, install the native build tools before configuring:

```sh
sudo apt install clang ninja-build cmake python3 python3-venv pkg-config \
  libvulkan-dev vulkan-validationlayers libx11-dev libxext-dev libxrandr-dev \
  libxcursor-dev libxi-dev libxfixes-dev libxkbcommon-dev libwayland-dev \
  libfreetype-dev libharfbuzz-dev xvfb
```

`xvfb` is used for automated window tests. A normal desktop session does not need it.

## Configure, build, test

```sh
python3 tools/fetch_slang.py
cmake --preset linux-debug
cmake --build --preset linux-debug --parallel
ctest --preset linux-debug
```

For an optimized build use `linux-release`. The `linux-sanitize` preset enables
AddressSanitizer and UndefinedBehaviorSanitizer for tests without the graphics backend.

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
LLVM `clang-cl`, Ninja, CMake, and the Vulkan SDK available. Then use the
`windows-debug` or `windows-release` presets.

```powershell
py tools/fetch_slang.py
cmake --preset windows-debug
cmake --build --preset windows-debug --parallel
ctest --preset windows-debug
```

Windows acceptance is tracked separately from Linux; a successful Linux build does
not verify a Windows build.
