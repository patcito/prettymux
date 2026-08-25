#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 1 ]; then
  echo "usage: $0 <new-prettymux.deb>" >&2
  exit 2
fi

new_deb="$(realpath "$1")"
new_version="$(dpkg-deb -f "$new_deb" Version)"
old_version="0.2.48"
test_root="$(mktemp -d)"
trap 'rm -rf "$test_root"' EXIT

dpkg --compare-versions "$new_version" gt "$old_version"

curl -fL \
  "https://github.com/patcito/prettymux/releases/download/v${old_version}/prettymux_${old_version}_amd64.deb" \
  -o "$test_root/prettymux-old.deb"

apt-get install -y "$test_root/prettymux-old.deb"
test "$(dpkg-query -W -f='${Version}' prettymux)" = "$old_version"

apt-get install -y "$new_deb"
test "$(dpkg-query -W -f='${Version}' prettymux)" = "$new_version"
test ! -e /usr/share/ghostty/themes/0x96f

fixture="$test_root/ghostty-fixture"
mkdir -p "$fixture/DEBIAN" "$fixture/usr/share/ghostty/themes"
printf '%s\n' \
  'Package: ghostty' \
  'Version: 1.2.3-0~ci1' \
  'Architecture: amd64' \
  'Maintainer: PrettyMux CI' \
  'Description: Ghostty ownership fixture for PrettyMux package tests' \
  > "$fixture/DEBIAN/control"
printf '%s\n' 'Ghostty-owned theme fixture' \
  > "$fixture/usr/share/ghostty/themes/0x96f"
dpkg-deb --build "$fixture" "$test_root/ghostty.deb"

apt-get install -y "$test_root/ghostty.deb"
dpkg-query -S /usr/share/ghostty/themes/0x96f | grep -q '^ghostty:'

dpkg -i "$new_deb"
test "$(dpkg-query -W -f='${Version}' prettymux)" = "$new_version"
dpkg-query -S /usr/share/ghostty/themes/0x96f | grep -q '^ghostty:'
dpkg-query -S /usr/share/prettymux/themes/0x96f | grep -q '^prettymux:'

echo "Debian upgrade and Ghostty coexistence test passed for PrettyMux $new_version"
