# Build and package PrettyMux

## Prerequisites

- GTK4, libadwaita, json-glib
- Meson + Ninja
- C17 compiler
- Ghostty fork from `patcito/ghostty` on branch `linux-embedded-platform`
- Zig `0.16.0+` to build `libghostty` (upstream's current `minimum_zig_version`)

### Install deps on Ubuntu or Debian

```bash
sudo apt-get install -y \
  libgtk-4-dev \
  libadwaita-1-dev \
  libjson-glib-dev \
  meson \
  ninja-build
```

### Install deps on Fedora

```bash
sudo dnf install -y \
  gtk4-devel \
  libadwaita-devel \
  json-glib-devel \
  meson \
  ninja-build
```

### Install deps on Arch

```bash
sudo pacman -S \
  gtk4 \
  libadwaita \
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

# Upstream installs the embedded library as ghostty-internal.so, but its
# SONAME is still libghostty.so -- which is the name PrettyMux links and the
# dynamic loader looks for at runtime. Create that name:
ln -sfn ghostty-internal.so zig-out/lib/libghostty.so
```

That produces:

- `zig-out/lib/ghostty-internal.so` (the shared library itself)
- `zig-out/lib/libghostty.so` (the symlink above; what PrettyMux links)
- `include/ghostty.h`

The Ghostty tree also needs:

- `vendor/glad/`

> **Watch out when updating Ghostty.** Older checkouts installed the library
> as `zig-out/lib/libghostty.so` directly. After upstream's rename, a stale
> `libghostty.so` left over from an older build will still satisfy both the
> link and the loader (identical SONAME), so PrettyMux silently keeps running
> the *old* terminal code even though the build "succeeded". If terminals
> behave like a previous version after an update, check that
> `zig-out/lib/libghostty.so` really resolves to the freshly built
> `ghostty-internal.so`:
>
> ```bash
> ls -l zig-out/lib/libghostty.so          # should point at ghostty-internal.so
> ldd builddir/prettymux | grep ghostty    # and prettymux should resolve to it
> ```

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
