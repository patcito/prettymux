# prettymux

prettymux is a native Linux terminal multiplexer built with GTK4, libadwaita, WebKit, and Ghostty. It combines vertical tabs, split panes, workspaces, a built-in browser, and agent-friendly notifications in one GPU-accelerated window.

Website: https://prettymux.com/

## Install

### Ubuntu 24.04

```bash
sudo apt update
sudo apt install -y ca-certificates
echo 'deb [trusted=yes arch=amd64] https://prettymux.com/apt/ubuntu noble main' | sudo tee /etc/apt/sources.list.d/prettymux.list
sudo apt update
sudo apt install prettymux
```

### Debian Stable

```bash
sudo apt update
sudo apt install -y ca-certificates
echo 'deb [trusted=yes arch=amd64] https://prettymux.com/apt/debian bookworm main' | sudo tee /etc/apt/sources.list.d/prettymux.list
sudo apt update
sudo apt install prettymux
```

### Debian Unstable

```bash
sudo apt update
sudo apt install -y ca-certificates
echo 'deb [trusted=yes arch=amd64] https://prettymux.com/apt/debian sid main' | sudo tee /etc/apt/sources.list.d/prettymux.list
sudo apt update
sudo apt install prettymux
```

### Fedora

```bash
sudo dnf config-manager addrepo --from-repofile=https://prettymux.com/rpm/fedora/prettymux.repo
sudo dnf install prettymux
```

### Arch Linux

```bash
yay -S prettymux
```

For the prebuilt package:

```bash
yay -S prettymux-bin
```

<!--
### Snap

```bash
sudo snap install prettymux
```

### Flatpak

```bash
flatpak remote-add --if-not-exists prettymux https://prettymux.com/flatpak/prettymux.flatpakrepo
flatpak install prettymux dev.prettymux.app
flatpak run dev.prettymux.app
```

### AppImage

```bash
wget https://github.com/patcito/prettymux/releases/download/v0.2.8/PrettyMux-0.2.8-x86_64.AppImage
chmod +x PrettyMux-0.2.8-x86_64.AppImage
./PrettyMux-0.2.8-x86_64.AppImage
```

## Build From Source

```bash
git clone https://github.com/patcito/prettymux
cd prettymux
meson setup builddir
ninja -C builddir
```

## License

This project is licensed under the GNU General Public License v3.0 only. See [LICENSE](LICENSE).

## Inspiration

PrettyMux was inspired by [cmux](https://github.com/manaflow-ai/cmux), a similar terminal multiplexer currently only available on macOS.
