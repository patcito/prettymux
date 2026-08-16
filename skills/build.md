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
```

That produces:

- `zig-out/lib/ghostty-internal.so` (upstream's name for the shared library)
- `zig-out/lib/libghostty.so` (same library under its SONAME; what PrettyMux
  links and what the dynamic loader resolves at runtime)
- `zig-out/share/ghostty/themes/` (the bundled color themes the terminal theme
  picker resolves against)
- `include/ghostty.h`

GLAD lives in the Ghostty tree under `vendor/glad/` and is compiled **into
libghostty**. PrettyMux deliberately does not compile it: it makes no GL calls
of its own, and a second copy in the executable would preempt libghostty's via
ELF symbol interposition.

> **Watch out when updating Ghostty.** Both files carry the SONAME
> `libghostty.so`, so a stale `zig-out/lib/libghostty.so` left over from an
> older build satisfies both the link and the loader — PrettyMux then silently
> keeps running the *old* terminal code even though the build "succeeded". If
> terminals behave like a previous version after an update, check the
> timestamps:
>
> ```bash
> ls -l zig-out/lib/libghostty.so          # should be from the build you just ran
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
