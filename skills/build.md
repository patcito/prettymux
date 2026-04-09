# Build and package PrettyMux

## Prerequisites

- GTK4, libadwaita, WebKitGTK 6.0, json-glib
- Meson + Ninja
- C17 compiler
- Ghostty fork from `patcito/ghostty` on branch `linux-embedded-platform`
- Zig `0.15.2+` to build `libghostty`

### Install deps on Ubuntu or Debian

```bash
sudo apt-get install -y \
  libgtk-4-dev \
  libadwaita-1-dev \
  libwebkitgtk-6.0-dev \
  libjson-glib-dev \
  meson \
  ninja-build
```

### Install deps on Fedora

```bash
sudo dnf install -y \
  gtk4-devel \
  libadwaita-devel \
  webkitgtk6.0-devel \
  json-glib-devel \
  meson \
  ninja-build
```

### Install deps on Arch

```bash
sudo pacman -S \
  gtk4 \
  libadwaita \
  webkit2gtk-6.0 \
  json-glib \
  meson \
  ninja
```

## Build `libghostty.so`

PrettyMux links against the embedded Ghostty fork. Build it with a generic Linux target:

```bash
cd /path/to/ghostty
zig build \
  -Dapp-runtime=none \
  -Doptimize=ReleaseFast \
  -Dtarget=x86_64-linux-gnu
```

That produces:

- `zig-out/lib/libghostty.so`
- `include/ghostty.h`

The Ghostty tree also needs:

- `vendor/glad/`

## Build PrettyMux

From the repo root:

```bash
cd src/gtk
meson setup builddir \
  --prefix=/usr \
  -Dghostty_dir=/path/to/ghostty
ninja -C builddir
```

For local development, if Ghostty is in the usual sibling path:

```bash
cd src/gtk
meson setup builddir --prefix=/usr
ninja -C builddir
```

## Run

```bash
./builddir/prettymux
```

## Packaging and release workflow

The main Linux packaging workflow is:

- `.github/workflows/release.yml`

Tagged pushes `v*` run the release build automatically.

Manual examples:

```bash
gh workflow run release.yml -f jobs=ubuntu
gh workflow run release.yml -f jobs=fedora
gh workflow run release.yml -f jobs=deb
gh workflow run release.yml -f jobs=arch
gh workflow run release.yml -f jobs=flatpak
gh workflow run release.yml -f jobs=appimage
gh workflow run release.yml -f jobs=linux
gh workflow run release.yml -f jobs=all
```

Available Linux job keywords:

- `ubuntu`
- `fedora`
- `deb`
- `arch`
- `snap`
- `flatpak`
- `appimage`
- `linux`
- `all`

Desktop builds are separate:

- `.github/workflows/desktop-platforms.yml`

Use that workflow for Windows and macOS artifacts.

## Packaging notes

- Build Ghostty with `-Dtarget=x86_64-linux-gnu` so shipped binaries do not pick unsupported CPU instructions.
- The Linux release workflow smoke-tests built packages and tolerates missing display errors, but not crashes like `Illegal instruction`, `Segmentation fault`, or `core dumped`.
- After `.deb`, `.rpm`, Flatpak, or AppImage builds succeed, sync the hosted repos in `../prettymux-web` so the website matches the latest release.
