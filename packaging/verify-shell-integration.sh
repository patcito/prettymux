#!/usr/bin/env bash
# Verify that the shell-integration tree was packaged correctly.
# Catches the case where dotfiles like .zshenv were dropped during
# packaging — without .zshenv, zsh users lose their .zshrc because
# ZDOTDIR never gets restored. See https://github.com/patcito/prettymux/issues/3
set -euo pipefail

if [ "$#" -lt 1 ]; then
  echo "usage: $0 <prettymux-share-dir>" >&2
  exit 2
fi

share_dir="$1"
shell_dir="$share_dir/shell-integration"

required=(
  "$shell_dir/zsh/.zshenv"
  "$shell_dir/zsh/ghostty-integration"
  "$shell_dir/bash/ghostty.bash"
  "$shell_dir/bash/bash-preexec.sh"
)

missing=0
for f in "${required[@]}"; do
  if [ ! -f "$f" ]; then
    echo "MISSING: $f" >&2
    missing=1
  fi
done

if [ "$missing" -ne 0 ]; then
  echo "shell-integration verification failed under $share_dir" >&2
  exit 1
fi

echo "shell-integration verified under $share_dir"
