# prettymux shell integration
# Source this in .bashrc or it gets auto-sourced via BASH_ENV

if [ -n "$PRETTYMUX" ] && [ -S "$PRETTYMUX_SOCKET" ]; then
    # Override xdg-open and open to route URLs to embedded browser
    xdg-open() {
        case "$1" in
            http://*|https://*)
                if [ -n "$PRETTYMUX_OPEN_BIN" ] && [ -x "$PRETTYMUX_OPEN_BIN" ]; then
                    "$PRETTYMUX_OPEN_BIN" "$1" >/dev/null 2>&1 && return 0
                fi
                ;;
        esac
        /usr/bin/xdg-open "$@"
    }
    open() { xdg-open "$@"; }
fi
