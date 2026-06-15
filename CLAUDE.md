# prettymux — agent guidance

prettymux is a **native Linux terminal multiplexer written in C (C17)**, built on
GTK4 + libadwaita with terminal surfaces provided by **libghostty** (OpenGL). It is
**not** a JavaScript/Bun project — ignore any generic Bun/Node guidance. There is no
web server, React, or bundler here.

## Build & test

Build system is **meson + ninja**. The source lives in `src/gtk/`; the build dir is
typically `builddir/` at the repo root.

```sh
# Configure (ghostty_dir defaults to ../ghostty via meson_options.txt)
meson setup builddir src/gtk --buildtype=release
# or point at a specific ghostty checkout:
#   meson setup builddir src/gtk -Dghostty_dir=/path/to/ghostty

ninja -C builddir            # build (produces builddir/prettymux + prettymux-open)
meson test -C builddir       # run the test suite (GTK4 headless; ~13 tests)
./builddir/prettymux         # run locally
```

`libghostty` must be built first (Zig). See `skills/build.md` for the full
from-source / packaging workflow and per-distro dependencies.

When you change code, **build with `ninja -C builddir` and run `meson test -C builddir`
before considering the work done.** New `*.c` files must be added to the `sources`
list in `src/gtk/meson.build`, and new tests registered there with `test(...)`.

## Layout (`src/gtk/`)

- `main.c` — app entry, window/overlay setup, shell-integration env, startup.
- `workspace.c` / `workspace_layout.c` / `workspace_strip.c` — workspaces, panes,
  tabs, and the two layout modes (classic split + horizontal "strip" of columns).
- `ghostty_terminal.c` — the GtkGLArea terminal widget wrapping a libghostty surface
  (GL realize/render, OSC handling, cwd/title/exit tracking).
- `ghostty_actions.c` — dispatch of libghostty actions (open URL, set title, progress…).
- `session.c` — JSON persistence of the workspace/pane/tab tree under `~/.prettymux`.
- `app_actions.c` / `shortcuts.c` / `shortcuts_overlay.c` — keybindings and actions.
- `command_palette.c`, `history.c`, `pane_move_overlay.c`, `resize_overlay.c` — overlays.
- `sidebar_ui.c` / `sidebar_sections.c` / `sidebar_data.c` — the workspace sidebar.
- `socket_server.c` / `socket_commands.c` — Unix-domain-socket IPC server.
- `prettymux-open.c` — standalone CLI client (`prettymux-open`) that talks to the socket.
- `prettymux_agent_cli.c` — agent-oriented CLI surface.
- `theme.c`, `notifications.c`, `app_settings.c`, `settings_dialog.c`, `app_state.c`.
- `prettymux-shell-integration.sh`, `prettymux-bashrc.sh` — injected shell integration
  (registers the terminal's session id, overrides `xdg-open` to forward URLs to the host).

## Conventions

- C17, GTK4 / libadwaita / GLib. Manual memory management with GLib refcounting
  (`g_object_ref`/`unref`, `g_ptr_array`, `g_free`, `g_autoptr`/`g_autofree`,
  `g_signal_connect`, `gtk_widget_unparent`). Match the surrounding style.
- All UI work runs on the GTK main thread.
- Tests are plain GLib test programs (`test_*.c`) run via meson; some need a display
  and skip headlessly — keep new tests display-optional where possible.
- IPC: the app listens on a per-instance Unix socket; `prettymux-open` and the shell
  integration send JSON commands. Keep client and server in sync when changing commands.

## Git

- Do not include AI attribution in commit messages.
- Commit/push only when asked.
