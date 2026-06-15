# prettymux socket IPC protocol

prettymux exposes a small JSON-over-Unix-socket API so external processes — the
`prettymux-open` CLI, shell integration, and **agents** — can drive and observe a
running instance. This document describes the wire protocol and the command
surface. The reference client is `prettymux-open` (run `prettymux-open --help`).

## Transport

- Each running instance listens on a per-user Unix domain socket:
  `$XDG_RUNTIME_DIR/prettymux-<instance-id>.sock` (falling back to
  `/tmp/prettymux-<instance-id>.sock` when `XDG_RUNTIME_DIR` is unset).
- A client connects, writes **one JSON object** (the request), and reads back
  **one JSON object** terminated by a newline (the response). The server closes
  the connection after replying.
- Processes started *inside* prettymux inherit `PRETTYMUX_SOCKET` (full path) and
  `PRETTYMUX_INSTANCE_ID`; clients should prefer `PRETTYMUX_SOCKET` and otherwise
  resolve the socket by instance id or by scanning the runtime dir.

### Targeting an instance

When several instances run, `prettymux-open` selects one in this order:
`--socket <path>`, `--instance <id>`, `PRETTYMUX_SOCKET`, `PRETTYMUX_INSTANCE_ID`,
else the single connectable instance found by scanning the runtime dir.
`prettymux-open --list-instances` enumerates them.

### Response envelope

Responses always include a status field:

```json
{ "status": "ok" }
{ "status": "error", "message": "invalid workspace index" }
```

Commands that return data add fields alongside `"status": "ok"` (e.g.
`workspace.list` adds `"workspaces": [...]`).

### Targeting fields (where applicable)

- `workspace` / `-w` — workspace index (0-based)
- `pane` / `-p` — pane index within the workspace
- `tab` / `-t` — tab index within the pane

## Command reference

Send `{"command": "<name>", ...fields}`. The `prettymux-open` flag that wraps each
command is shown in parentheses.

### Terminals & input
| Command | Fields | Purpose |
|---|---|---|
| `open.url` (`<url>`) | `url` | Open a URL with the host's default handler |
| `exec` (`--exec`) | `cmd`, `workspace`,`pane`,`tab` | Type a command + Enter into a terminal |
| `type` (`--type`) | `text`, `workspace`,`pane`,`tab` | Type text into a terminal (no Enter) |
| `pane.read_text` | `workspace`,`pane`,`tab` | Read the terminal's visible/scrollback text |
| `action` (`--action`) | `action` (e.g. `split.vertical`), `nonInteractive` | Run any keybinding action by name |
| `list.actions` (`--list-actions`) | — | List every action name |

### Workspaces
| Command | Fields | Purpose |
|---|---|---|
| `workspace.new` (`--new-workspace`) | `name` (optional) | Create a workspace |
| `workspace.list` (`--list-workspaces`) | — | List workspaces |
| `workspace.current` | — | The focused workspace |
| `workspace.switch` (`--switch-workspace`) | `index` | Focus workspace N |
| `workspace.edit` (`--edit-workspace`) | `workspace` | Begin inline rename |
| `workspace.set_layout` (`--set-layout`) | `layout` (`classic`/`strip`), `workspace` (omit = all) | Set layout mode |
| `workspace.get_layout` (`--get-layout`) | `workspace` | Get layout mode |
| `workspace.get_strip_state` (`--get-strip-state`) | `workspace` | Strip columns/focus/maximize state |
| `workspace.equalize_splits` | `workspace` | Even out split sizes |
| `workspace.import` | `workspacePayload` (serialized workspace JSON) | Load a workspace from JSON |
| `workspace.move_to_instance` (`--move-workspace`) | `targetInstanceId`, `workspace` | Hand a workspace to another instance |

### Panes & tabs
| Command | Fields | Purpose |
|---|---|---|
| `pane.split` | `workspace`,`pane`, direction | Split a pane |
| `pane.close` | `workspace`,`pane` | Close a pane |
| `pane.focus` | `workspace`,`pane` | Focus a pane |
| `pane.list` | `workspace` | List panes |
| `pane.resize_percent` | `workspace`,`pane`, percent | Resize a split |
| `tab.new` (`--new-tab`) | — | New terminal tab in the focused pane |
| `tab.select` (`--select-tab`) | `workspace`,`pane`,`tab` | Focus a tab |
| `tab.rename` | `name` | Rename the focused tab |
| `tab.edit` (`--edit-tab`) | — | Begin inline rename of the focused tab |
| `tab.move` (`--move-tab`) | `from*`, `to*` indices | Move a tab between panes |
| `tabs.list` (`--list-tabs`) | — | List workspaces → panes → tabs |

### Lifecycle / misc
| Command | Fields | Purpose |
|---|---|---|
| `app.quit` (`--quit`) | — | Close prettymux cleanly |
| `dismiss.welcome` | — | Dismiss the welcome screen |
| `terminal.register` (`--register-terminal`) | `terminalId`, `sessionId` | Shell integration registers a terminal's session id (used for clean shutdown SIGHUP) |

## Agent notifications (`workspace.status.*`)

Agents can attach structured **status entries** to a workspace — surfaced in the
sidebar and (optionally) as desktop notifications. This is the feature behind
prettymux's "agent-friendly notifications".

### `workspace.status.set` (`--set-workspace-status`)

Create or update an entry, keyed by `entryId`.

| Field | Type | Notes |
|---|---|---|
| `entryId` | string | **required** — stable key; re-setting updates in place |
| `summary` | string | short one-line summary |
| `provider` | string | e.g. the agent/tool name |
| `kind` | string | caller-defined category |
| `state` | string | caller-defined state (e.g. `running`, `done`, `failed`) |
| `detail` | string | longer detail text |
| `attention` | bool | mark as needing attention |
| `notify` | bool | also emit a desktop notification |
| `updatedAtUsec` / `updatedAtMs` | int | timestamp (defaults to now) |
| `workspace` | int | target workspace (defaults to current) |

Request / response:

```json
{ "command": "workspace.status.set", "entryId": "deploy:main", "provider": "ci",
  "kind": "deploy", "state": "running", "summary": "Deploying main",
  "detail": "rolling out 5 instances", "attention": false, "notify": true, "workspace": 0 }
```
```json
{ "status": "ok", "workspace": 0, "entryId": "deploy:main" }
```

CLI:

```sh
prettymux-open --set-workspace-status --id deploy:main --provider ci \
  --kind deploy --state running --summary "Deploying main" \
  --detail "rolling out 5 instances" --notify -w 0
```

### `workspace.status.clear` (`--clear-workspace-status`)

Remove an entry by id: `{ "command": "workspace.status.clear", "entryId": "deploy:main", "workspace": 0 }`.

### `workspace.status.list` (`--list-workspace-status`)

List a workspace's status entries.

## Notes

- This reference is maintained by hand; the authoritative command surface is the
  dispatcher in `src/gtk/socket_commands.c` and `prettymux-open --help`. When
  adding or changing a command, update both.
