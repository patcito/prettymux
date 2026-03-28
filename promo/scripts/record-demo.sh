#!/bin/bash
# Record a prettymux demo using the socket API + ffmpeg
# Run this from INSIDE a prettymux terminal so prettymux is in the foreground
set -e

PROMO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUTPUT="$PROMO_DIR/public/demo-raw.mp4"
GTK_DIR="$PROMO_DIR/../src/gtk"
PMUX_OPEN="$GTK_DIR/builddir/prettymux-open"

# Find socket
if [ -z "$PRETTYMUX_SOCKET" ]; then
    PRETTYMUX_SOCKET=$(ls -t /tmp/prettymux-*.sock 2>/dev/null | head -1)
fi
if [ -z "$PRETTYMUX_SOCKET" ]; then
    echo "ERROR: No prettymux socket found. Run from inside prettymux."
    exit 1
fi
export PRETTYMUX_SOCKET
echo "Socket: $PRETTYMUX_SOCKET"

# Verify connection
"$PMUX_OPEN" --list-workspaces >/dev/null 2>&1 || {
    echo "ERROR: Can't connect to prettymux socket"
    exit 1
}

# ── Helpers ──

pm() { "$PMUX_OPEN" "$@" >/dev/null 2>&1; }

# Type text character by character with realistic delay
typeslow() {
    local text="$1"
    local delay="${2:-0.04}"
    local i
    for (( i=0; i<${#text}; i++ )); do
        local char="${text:$i:1}"
        "$PMUX_OPEN" --type "$char" >/dev/null 2>&1
        sleep "$delay"
    done
}

# Type text then press enter
typecmd() {
    typeslow "$1" "${2:-0.04}"
    sleep 0.1
    "$PMUX_OPEN" --type $'\n' >/dev/null 2>&1
}

# Type fast (for commands we don't want to watch)
typefast() {
    "$PMUX_OPEN" --type "$1" >/dev/null 2>&1
    sleep 0.05
    "$PMUX_OPEN" --type $'\n' >/dev/null 2>&1
}

# Targeting helpers
typeslow_at() {
    local text="$1"
    local ws="$2"
    local pane="$3"
    local tab="$4"
    local delay="${5:-0.04}"
    local i
    for (( i=0; i<${#text}; i++ )); do
        local char="${text:$i:1}"
        "$PMUX_OPEN" --type "$char" -w "$ws" -p "$pane" -t "$tab" >/dev/null 2>&1
        sleep "$delay"
    done
}

typecmd_at() {
    typeslow_at "$1" "$2" "$3" "$4" "${5:-0.04}"
    sleep 0.1
    "$PMUX_OPEN" --type $'\n' -w "$2" -p "$3" -t "$4" >/dev/null 2>&1
}

# ── Screen setup ──

SCREEN_RES=$(xdpyinfo 2>/dev/null | grep dimensions | awk '{print $2}' || echo "1920x1080")
echo "Screen: $SCREEN_RES"
echo "Output: $OUTPUT"
echo ""
echo "Make sure PrettyMux is the FOREGROUND window!"
echo "Starting in 3 seconds..."
sleep 3

# ── Start recording ──

mkdir -p "$(dirname "$OUTPUT")"
mkfifo /tmp/ffmpeg_pipe 2>/dev/null || true

ffmpeg -y -f x11grab -framerate 30 \
    -video_size "$SCREEN_RES" \
    -i "$DISPLAY+0,0" \
    -c:v libx264 -preset ultrafast -crf 18 \
    -pix_fmt yuv420p \
    "$OUTPUT" < /tmp/ffmpeg_pipe 2>/tmp/ffmpeg.log &
FFMPEG_PID=$!
exec 3>/tmp/ffmpeg_pipe
sleep 1

echo "=== Recording started ==="

# ── SCENE 1: Intro typing (5s) ──
typefast "clear"
sleep 0.5
typecmd "echo ''" 0.02
typeslow "echo '  Welcome to PrettyMux'" 0.05
"$PMUX_OPEN" --type $'\n' >/dev/null 2>&1
sleep 0.8
typeslow "echo '  GPU-accelerated terminal multiplexer'" 0.04
"$PMUX_OPEN" --type $'\n' >/dev/null 2>&1
sleep 0.8
typeslow "echo '  ghostty + WebKit in one window'" 0.04
"$PMUX_OPEN" --type $'\n' >/dev/null 2>&1
sleep 1.5

# ── SCENE 2: ls and basic usage (3s) ──
typecmd "ls -la --color" 0.05
sleep 2

# ── SCENE 3: Create new tab (2s) ──
pm --action newtab
sleep 0.8
typecmd "echo 'New tab created!'" 0.04
sleep 1

# ── SCENE 4: Horizontal split (3s) ──
pm --action hsplit
sleep 1
typeslow "echo 'Horizontal split'" 0.05
"$PMUX_OPEN" --type $'\n' >/dev/null 2>&1
sleep 1.5

# ── SCENE 5: Vertical split (3s) ──
pm --action vsplit
sleep 1
typeslow "echo 'Vertical split'" 0.05
"$PMUX_OPEN" --type $'\n' >/dev/null 2>&1
sleep 1.5

# ── SCENE 6: Type in different panes (4s) ──
typecmd_at "echo 'Pane 1'" 0 0 0 0.06
sleep 0.5
typecmd_at "echo 'Pane 2'" 0 1 0 0.06
sleep 0.5
typecmd_at "echo 'Pane 3'" 0 2 0 0.06
sleep 1.5

# ── SCENE 7: New workspace (3s) ──
pm --new-workspace "dev-server"
sleep 0.8
typeslow "echo 'New workspace: dev-server'" 0.04
"$PMUX_OPEN" --type $'\n' >/dev/null 2>&1
sleep 1.5

# ── SCENE 8: Switch workspaces (2s) ──
pm --switch-workspace 0
sleep 0.7
pm --switch-workspace 1
sleep 0.7
pm --switch-workspace 0
sleep 0.5

# ── SCENE 9: Open browser (4s) ──
pm --action browser
sleep 0.8
pm "https://github.com/patcito/prettymux"
sleep 3

# ── SCENE 10: Shortcuts overlay (3s) ──
pm --action shortcuts
sleep 3
pm --action shortcuts
sleep 0.5

# ── SCENE 11: Theme cycling (5s) ──
pm --action theme
sleep 1.5
pm --action theme
sleep 1.5
pm --action theme
sleep 1.5

# ── SCENE 12: Command palette (2s) ──
pm --action palette
sleep 2
# Close it
pm --action palette
sleep 0.5

# ── SCENE 13: Zoom pane (3s) ──
pm --action zoom
sleep 2
pm --action zoom
sleep 1

# ── SCENE 14: Final typing (3s) ──
typeslow "echo 'prettymux — terminal multiplexer for the modern era'" 0.03
"$PMUX_OPEN" --type $'\n' >/dev/null 2>&1
sleep 2

# ── Stop recording ──
echo "q" >&3
exec 3>&-
sleep 2
kill $FFMPEG_PID 2>/dev/null
wait $FFMPEG_PID 2>/dev/null || true
rm -f /tmp/ffmpeg_pipe

echo ""
echo "=== Recording complete ==="
ls -lh "$OUTPUT"
ffprobe "$OUTPUT" 2>&1 | grep -E "Duration|Video" || true
echo ""
echo "Next: cd promo && bun run build to create the final video"
