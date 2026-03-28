# prettymux shell integration
# Source this in .bashrc or it gets auto-sourced via BASH_ENV

if [ -n "$PRETTYMUX" ] && [ -S "$PRETTYMUX_SOCKET" ]; then
    # Override xdg-open and open to route URLs to embedded browser
    xdg-open() {
        case "$1" in
            http://*|https://*)
                echo "{\"command\":\"browser.open\",\"url\":\"$1\"}" | socat - UNIX-CONNECT:"$PRETTYMUX_SOCKET" 2>/dev/null && return 0
                ;;
        esac
        /usr/bin/xdg-open "$@"
    }
    open() { xdg-open "$@"; }
fi
