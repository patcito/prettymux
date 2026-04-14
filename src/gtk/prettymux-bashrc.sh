#!/bin/sh

if [ -z "${BASH_VERSION-}" ]; then
    return 0 2>/dev/null || exit 0
fi

if [ -n "$PRETTYMUX_GHOSTTY_BASH_INTEGRATION" ] &&
   [ -r "$PRETTYMUX_GHOSTTY_BASH_INTEGRATION" ] &&
   ! command -V __ghostty_precmd >/dev/null 2>&1; then
    . "$PRETTYMUX_GHOSTTY_BASH_INTEGRATION"
fi

if [ -r "$HOME/.bashrc" ]; then
    . "$HOME/.bashrc"
fi

if [ -n "$PRETTYMUX_SHELL_INTEGRATION" ] &&
   [ -r "$PRETTYMUX_SHELL_INTEGRATION" ]; then
    . "$PRETTYMUX_SHELL_INTEGRATION"
fi
