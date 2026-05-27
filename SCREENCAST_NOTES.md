# Prettymux Screencast Notes

## Recording

- Shortcut log: `/home/pe/.local/state/prettymux/shortcuts.jsonl`
- Recording marker: `Ctrl+Shift+Y`
- Copy cue: `Ctrl+Shift+C`

## Demo Shortcuts

- `Ctrl+Shift+Y` — Recording marker
- `Ctrl+Shift+O` — Split vertical
- `Ctrl+Shift+E` — Split horizontal
- `Alt+Left/Right/Up/Down` — Focus pane
- `Ctrl+Shift+Enter` — Broadcast mode
- `Ctrl+Shift+Z` — Zoom pane
- `Ctrl+Shift+T` — New terminal tab
- `Ctrl+Shift+G` — Move tab to pane
- `Ctrl+Shift+W` — Close tab
- `Ctrl+Shift+X` — Close pane
- `Ctrl+Shift+N` — New workspace
- `Ctrl+Shift+D` — Close workspace
- `Ctrl+Shift+[` — Previous workspace
- `Ctrl+Shift+]` — Next workspace
- `Ctrl+Shift+1..9` — Jump to workspace
- `Ctrl+Shift+S` — Search palette
- `Ctrl+Shift+H` — Command history
- `Ctrl+Shift+F` — Terminal search
- `Ctrl+Shift+Q` — Quick notes
- `Ctrl+Alt+S` — Settings
- `Ctrl+Shift+,` — Cycle theme
- `Ctrl+Shift+K` — Shortcuts overlay
- `F11` — Fullscreen

## Recorded Timeline

- `00:00.000` Recording start
- `00:01.377` Split horizontal
- `00:02.630` Split vertical
- `00:04.508` Broadcast mode on
- `00:08.768` Broadcast mode off
- `00:35.682` Focus pane up
- `00:36.302` Focus pane left
- `00:36.855` Focus pane right
- `00:39.836` Zoom pane
- `00:42.309` Unzoom pane
- `00:47.387` Focus pane left
- `00:48.517` New terminal tab
- `00:53.26` Move tab inside pane by dragging
- `00:56.16` Move tab from one pane to another by dragging
- `01:04.077` New workspace
- `01:22.853` New terminal tab
- `01:26.589` Search palette
- `01:43.647` Jump to workspace 1
- `01:55.302` Search palette
- `01:58.043` Shortcuts overlay
- `02:02.475` Settings
- `02:12.255` Cycle theme
- `02:13.204` Cycle theme
- `02:13.843` Cycle theme
- `02:14.600` Cycle theme
- `02:16.724` Jump to workspace 1
- `02:22.191` Quick notes on
- `02:27.635` Quick notes off
- `02:50.403` Terminal search

## Notes

- The second `broadcast.toggle` event is the exit from broadcast mode.
- No `tab.drag.move` event appears in this take.
- The two drag events above were added manually because they were missing from the JSONL log.
