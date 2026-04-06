# prettymux

prettymux is a native Linux terminal multiplexer built with GTK4, libadwaita, WebKit, and Ghostty. It combines vertical tabs, split panes, workspaces, a built-in browser, and agent-friendly notifications in one GPU-accelerated window.

Website: https://prettymux.com/

## Install

### Ubuntu 24.04

```bash
sudo add-apt-repository "deb [trusted=yes] https://prettymux.com/apt/ubuntu noble main"
sudo apt update
sudo apt install prettymux
```

### Debian Stable

```bash
sudo add-apt-repository "deb [trusted=yes] https://prettymux.com/apt/debian bookworm main"
sudo apt update
sudo apt install prettymux
```

### Debian Unstable

```bash
sudo add-apt-repository "deb [trusted=yes] https://prettymux.com/apt/debian sid main"
sudo apt update
sudo apt install prettymux
```

### Fedora

```bash
sudo dnf install https://github.com/patcito/prettymux/releases/download/v0.2.6/prettymux-0.2.6-1.x86_64.rpm
```

### Arch Linux

```bash
yay -S prettymux
```

For the prebuilt package:

```bash
yay -S prettymux-bin
```

### Snap

```bash
sudo snap install prettymux
```

### Flatpak

```bash
flatpak install flathub dev.prettymux.app
flatpak run dev.prettymux.app
```

### AppImage

```bash
wget https://github.com/patcito/prettymux/releases/latest/download/PrettyMux-x86_64.AppImage
chmod +x PrettyMux-x86_64.AppImage
./PrettyMux-x86_64.AppImage
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
