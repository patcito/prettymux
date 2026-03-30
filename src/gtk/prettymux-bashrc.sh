#!/bin/sh

if [ -r "$HOME/.bashrc" ]; then
    . "$HOME/.bashrc"
fi

if [ -n "$PRETTYMUX_SHELL_INTEGRATION" ] &&
   [ -r "$PRETTYMUX_SHELL_INTEGRATION" ]; then
    . "$PRETTYMUX_SHELL_INTEGRATION"
fi
