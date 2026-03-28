# Build prettymux

## Prerequisites

- GTK4, libadwaita, WebKitGTK 6.0, json-glib
- Meson + Ninja
- C17 compiler
- ghostty fork (branch `linux-embedded-platform` from `patcito/ghostty`)
- GLAD (vendored in ghostty at `vendor/glad/`)

### Install deps (Ubuntu/Debian)
```bash
sudo apt-get install -y libgtk-4-dev libadwaita-1-dev libwebkitgtk-6.0-dev libjson-glib-dev meson ninja-build
```

### Install deps (Fedora)
```bash
sudo dnf install -y gtk4-devel libadwaita-devel webkitgtk6.0-devel json-glib-devel meson ninja-build
```

### Install deps (Arch)
```bash
sudo pacman -S gtk4 libadwaita webkit2gtk-6.0 json-glib meson ninja
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
cd src/gtk
meson setup builddir -Dghostty_dir=/path/to/ghostty
ninja -C builddir
```

The `ghostty_dir` should point to the ghostty directory containing:
- `zig-out/lib/libghostty.so`
- `include/ghostty.h`
- `vendor/glad/`

For local development (ghostty at default path):
```bash
cd src/gtk && meson setup builddir && ninja -C builddir
```

## Run

```bash
./builddir/prettymux
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
