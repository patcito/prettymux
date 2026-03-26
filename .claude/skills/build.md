# Build prettymux

## Prerequisites

- Qt6 (Widgets, WebEngineWidgets, OpenGLWidgets)
- CMake 3.16+
- C++17 compiler
- ghostty fork (branch `linux-embedded-platform` from `patcito/ghostty`)
- GLAD (vendored in ghostty at `vendor/glad/`)

### Install deps (Ubuntu/Debian)
```bash
sudo apt-get install -y qt6-base-dev qt6-webengine-dev libgl-dev cmake g++
```

### Install deps (Fedora)
```bash
sudo dnf install -y qt6-qtbase-devel qt6-qtwebengine-devel mesa-libGL-devel cmake gcc-c++
```

### Install deps (Arch)
```bash
sudo pacman -S qt6-base qt6-webengine mesa cmake gcc make
```

## Build ghostty (libghostty.so)

Requires Zig 0.15.2+.

```bash
cd /path/to/ghostty
zig build -Dapp-runtime=none -Doptimize=ReleaseFast
```

This produces `zig-out/lib/libghostty.so` and headers at `include/ghostty.h`.

Prebuilt binaries for Linux x86_64 are available at:
https://github.com/patcito/ghostty/releases/tag/linux-embedded-v0.1.0

## Build prettymux

```bash
cd src/qt
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DGHOSTTY_DIR=/path/to/ghostty
make -j$(nproc)
```

The `GHOSTTY_DIR` should point to the ghostty directory containing:
- `zig-out/lib/libghostty.so`
- `include/ghostty.h`
- `vendor/glad/`

For local development (ghostty at default path):
```bash
cd src/qt/build && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j$(nproc)
```

## Run

```bash
LD_LIBRARY_PATH=/path/to/ghostty/zig-out/lib ./prettymux
```

## CI

Builds are triggered manually via GitHub Actions:

```bash
# Run specific jobs
gh workflow run release.yml -f jobs=fedora
gh workflow run release.yml -f jobs="ubuntu,deb"
gh workflow run release.yml -f jobs=linux      # all Linux builds
gh workflow run release.yml -f jobs=macos
gh workflow run release.yml -f jobs=all

# Tag push (v*) triggers all jobs automatically
```

Available job keywords: `ubuntu`, `fedora`, `deb`, `arch`, `snap`, `flatpak`, `appimage`, `windows`, `macos`, `ppa`, `linux` (all Linux), `all`.
