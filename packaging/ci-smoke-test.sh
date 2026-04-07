#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 2 ]; then
  echo "usage: $0 <label> <command> [args...]" >&2
  exit 2
fi

label="$1"
shift

log_file="$(mktemp)"
trap 'rm -f "$log_file"' EXIT

set +e
timeout 10s "$@" >"$log_file" 2>&1
status=$?
set -e

cat "$log_file"

if grep -Eiq '(illegal instruction|segmentation fault|trace/breakpoint trap|bus error|core dumped)' "$log_file"; then
  echo "$label smoke test crashed" >&2
  exit 1
fi

case "$status" in
  0)
    exit 0
    ;;
  124|137)
    echo "$label smoke test reached timeout without crashing"
    exit 0
    ;;
esac

if grep -Eiq '(cannot open display|unable to init server|no protocol specified|failed to open display|cannot connect to display|wayland display|x11 display|gtk-warning.*display)' "$log_file"; then
  echo "$label smoke test hit the expected headless display failure"
  exit 0
fi

echo "$label smoke test failed unexpectedly with exit code $status" >&2
exit 1
