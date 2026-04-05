# prettymux shell integration
# Source this in .bashrc or it gets auto-sourced via BASH_ENV

if [ -n "$PRETTYMUX" ] && [ -S "$PRETTYMUX_SOCKET" ]; then
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

case "$-" in
    *i*) _prettymux_shell_is_interactive=1 ;;
    *) _prettymux_shell_is_interactive=0 ;;
esac

if [ -n "$BASH_VERSION" ] &&
   [ "$_prettymux_shell_is_interactive" = "1" ] &&
   [ -z "$PRETTYMUX_GHOSTTY_SHELL_LOADED" ]; then
    export PRETTYMUX_GHOSTTY_SHELL_LOADED=1

    if [ -n "$PRETTYMUX_GHOSTTY_BASH_INTEGRATION" ] &&
       [ -r "$PRETTYMUX_GHOSTTY_BASH_INTEGRATION" ]; then
        . "$PRETTYMUX_GHOSTTY_BASH_INTEGRATION"
    fi
fi

if [ -n "$BASH_VERSION" ] &&
   [ "$_prettymux_shell_is_interactive" = "1" ] &&
   [ -n "$PRETTYMUX" ] &&
   [ -S "$PRETTYMUX_SOCKET" ] &&
   [ -n "$PRETTYMUX_OPEN_BIN" ] &&
   [ -x "$PRETTYMUX_OPEN_BIN" ] &&
   [ -n "$PRETTYMUX_TERMINAL_ID" ] &&
   [ -z "$PRETTYMUX_TERMINAL_REGISTERED" ]; then
    PRETTYMUX_TERMINAL_REGISTERED=1
    export PRETTYMUX_TERMINAL_REGISTERED

    _prettymux_session_id="$(ps -o sid= -p "$$" 2>/dev/null | tr -d '[:space:]')"
    _prettymux_tty_name="$(ps -o tty= -p "$$" 2>/dev/null | tr -d '[:space:]')"
    _prettymux_tty_path="$(readlink -f "/proc/$$/fd/0" 2>/dev/null)"

    if [ -n "$_prettymux_session_id" ]; then
        "$PRETTYMUX_OPEN_BIN" \
            --register-terminal "$PRETTYMUX_TERMINAL_ID" \
            --session-id "$_prettymux_session_id" \
            --tty-name "${_prettymux_tty_name:-}" \
            --tty-path "${_prettymux_tty_path:-}" \
            >/dev/null 2>&1 || true
    fi

    unset _prettymux_session_id
    unset _prettymux_tty_name
    unset _prettymux_tty_path
fi

unset _prettymux_shell_is_interactive
