# prettymux

prettymux is a native Linux terminal multiplexer built with GTK4, libadwaita, WebKit, and Ghostty. It combines vertical tabs, split panes, workspaces, a built-in browser, and agent-friendly notifications in one GPU-accelerated window.

Website: https://prettymux.com/

## Install

### Ubuntu 24.04

```bash
sudo apt update
sudo apt install -y ca-certificates lsb-release
echo "deb [trusted=yes arch=amd64] https://prettymux.com/apt/ubuntu $(lsb_release -cs 2>/dev/null) main" | sudo tee /etc/apt/sources.list.d/prettymux.list
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

### macOS Beta

```sh
curl -L -o PrettyMux-macos-arm64.dmg \
  https://github.com/patcito/prettymux/releases/latest/download/PrettyMux-macos-arm64.dmg
open PrettyMux-macos-arm64.dmg
```

### Windows Beta

```powershell
Invoke-WebRequest https://github.com/patcito/prettymux/releases/latest/download/PrettyMux-Installer-x64.msi -OutFile PrettyMux-Installer-x64.msi
Start-Process msiexec.exe -Wait -ArgumentList '/i', 'PrettyMux-Installer-x64.msi'
```

## Screenshots

### Terminal Splitting and Workspace Tabs

![Terminal Splitting and Workspace Tabs](https://prettymux.com/screenshots/terminal-splitting.jpg)

### Browser Integration

![Browser Integration](https://prettymux.com/screenshots/browser-integration.jpg)

### Search Palette

![Search Palette](https://prettymux.com/screenshots/search-palette.jpg)

### Shortcuts Overlay

![Shortcuts Overlay](https://prettymux.com/screenshots/shortcuts-overlay.jpg)

### Theme Cycling

![Theme Cycling](https://prettymux.com/screenshots/theme-cycling.jpg)

### Broadcast Mode

![Broadcast Mode](https://prettymux.com/screenshots/broadcast-mode.jpg)

### Quick Notes

![Quick Notes](https://prettymux.com/screenshots/quick-notes.jpg)

## Automation with `prettymux-open`

`prettymux-open` lets you script a running PrettyMux instance from the shell. You can open URLs, trigger actions, create workspaces, run commands in terminals, and move tabs between panes.

```bash
# Open a URL
prettymux-open https://prettymux.com

# Trigger an action
prettymux-open --action split.vertical

# Create a workspace and run a command in a specific tab
prettymux-open --new-workspace api
prettymux-open --exec "bun run dev" -w 0 -p 0 -t 0

# Move a tab to another pane
prettymux-open --move-tab --from-w 0 --from-p 0 --from-t 1 --to-w 1 --to-p 0
```

Run `prettymux-open --list-actions`, `prettymux-open --list-workspaces`, or `prettymux-open --list-tabs` to inspect what can be automated.

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
wget https://prettymux.com/appimage/PrettyMux-x86_64.AppImage
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

## Inspiration

PrettyMux was inspired by [cmux](https://github.com/manaflow-ai/cmux), a similar terminal multiplexer currently only available on macOS.
