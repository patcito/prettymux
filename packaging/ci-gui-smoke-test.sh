#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 2 ]; then
  echo "usage: $0 <label> <command> [args...]" >&2
  exit 2
fi

label="$1"
shift

test_root="$(mktemp -d)"
log_file="$test_root/prettymux.log"
mkdir -p "$test_root/home" "$test_root/config" "$test_root/cache" \
  "$test_root/data" "$test_root/state" "$test_root/runtime"
chmod 700 "$test_root/runtime"
trap 'rm -rf "$test_root"' EXIT

set +e
timeout --signal=TERM --kill-after=5s 20s \
  dbus-run-session -- \
  xvfb-run -a -s '-screen 0 1280x800x24 -nolisten tcp +extension GLX +render -noreset' \
  env \
    HOME="$test_root/home" \
    XDG_CONFIG_HOME="$test_root/config" \
    XDG_CACHE_HOME="$test_root/cache" \
    XDG_DATA_HOME="$test_root/data" \
    XDG_STATE_HOME="$test_root/state" \
    XDG_RUNTIME_DIR="$test_root/runtime" \
    GDK_BACKEND=x11 \
    LIBGL_ALWAYS_SOFTWARE=1 \
    GSETTINGS_BACKEND=memory \
    NO_AT_BRIDGE=1 \
    PRETTYMUX_INSTANCE_ID="ci-${label}-$$" \
    "$@" >"$log_file" 2>&1
status=$?
set -e

cat "$log_file"

if grep -Eiq '(illegal instruction|segmentation fault|trace/breakpoint trap|bus error|core dumped)' "$log_file"; then
  echo "$label GUI smoke test crashed" >&2
  exit 1
fi

if grep -Eiq '(theme parser error|no property named)' "$log_file"; then
  echo "$label GUI smoke test reported invalid GTK CSS" >&2
  exit 1
fi

case "$status" in
  124|137)
    echo "$label installed package stayed alive with a real X11/OpenGL surface"
    exit 0
    ;;
  0)
    echo "$label GUI smoke test exited before the timeout" >&2
    exit 1
    ;;
  *)
    echo "$label GUI smoke test failed with exit code $status" >&2
    exit 1
    ;;
esac
