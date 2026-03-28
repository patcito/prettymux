/**
 * Smithers workflow: PrettyMux power features
 *
 * Terminal power features, productivity tools, and quality of life improvements.
 *
 * Usage:
 *   bunx smithers run workflows/power-features.tsx
 *
 * Phase 1: Terminal power features (Alt+Arrow focus, pane zoom, terminal search, broadcast mode)
 *          + Activity indicator, progress bar from ghostty actions
 * Phase 2: Productivity (git status sidebar, port scanner, quick notes)
 *          + Custom keyboard shortcuts with inline editing in overlay
 * Phase 3: Quality of life (drag & drop tabs, theme switcher, command history, picture-in-picture)
 *
 * Delete smithers.db before running to start fresh.
 */
import { createSmithers, Ralph, Sequence } from "smithers-orchestrator";
import { ClaudeCodeAgent, CodexAgent } from "smithers-orchestrator";
import { z } from "zod";

const implResult = z.object({
  summary: z.string(),
  filesChanged: z.array(z.string()),
});

const reviewResult = z.object({
  approved: z.boolean(),
  issues: z.array(z.string()),
  summary: z.string(),
});

const { Workflow, Task, smithers, outputs } = createSmithers({
  impl1: implResult,
  review1: reviewResult,
  impl2: implResult,
  review2: reviewResult,
  impl3: implResult,
  review3: reviewResult,
});

const PROJECT_DIR = "/home/pe/newnewrepos/w/yo/prettymux";
const BUILD_CMD = "cd src/qt/build && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j$(nproc)";

const coder = new ClaudeCodeAgent({
  model: "claude-opus-4-6",
  dangerouslySkipPermissions: true,
  allowDangerouslySkipPermissions: true,
});

const reviewer = new CodexAgent({
  model: "gpt-5.3-codex",
  config: { model_reasoning_effort: "high" },
  dangerouslyBypassApprovalsAndSandbox: true,
  skipGitRepoCheck: true,
  cd: PROJECT_DIR,
});

export default smithers((ctx) => (
  <Workflow name="prettymux-power-features">
    <Sequence>

      {/* ═══════════════════════════════════════════════════════════════
          Phase 1: Terminal Power Features + Ghostty Activity/Progress
          ═══════════════════════════════════════════════════════════════ */}
      <Ralph
        until={ctx.latest(reviewResult, "review1")?.approved}
        maxIterations={3}
        onMaxReached="return-last"
      >
        <Sequence>
        <Task id="impl1" output={implResult} agent={coder} retries={2}>
          {`You are working on PrettyMux at ${PROJECT_DIR}.
Build with: ${BUILD_CMD}

This is a GTK4 app. Main source: src/qt/main.cpp
It has workspaces with PaneWidgets, each pane has tabs with GhosttyWidget terminals.
Read src/qt/main.cpp FIRST to understand the full codebase.

The ghostty C API is at /home/pe/newnewrepos/w/yo/ghostty/include/ghostty.h
Key ghostty functions: ghostty_surface_key, ghostty_surface_text, ghostty_surface_binding_action
Key ghostty actions in the action_cb: GHOSTTY_ACTION_RENDER, GHOSTTY_ACTION_PROGRESS_REPORT

${ctx.latest(reviewResult, "review1")?.issues?.length ? `\nPREVIOUS REVIEW FEEDBACK:\n${ctx.latest(reviewResult, "review1")!.issues.join("\n")}\n` : ""}

Implement PHASE 1: Terminal Power Features + Activity/Progress

1. ALT+ARROW FOCUS NAVIGATION:
   In PrettyMuxWindow::keyPressEvent, add Alt+Arrow key handling.
   When Alt+Left/Right/Up/Down is pressed, find the PaneWidget
   geometrically nearest in that direction from the currently focused pane.
   Use QWidget::mapToGlobal to compare pane positions.
   Call setFocus on the first terminal in the target pane.
   Intercept Alt+Arrow in GhosttyWidget before ghostty gets it.

2. PANE ZOOM:
   Add Ctrl+Shift+Z to toggle zoom on the focused pane.
   When zooming: hide all other panes in the workspace, show only the zoomed one.
   Store the original splitter sizes. When unzooming, restore them.
   Add a "zoomed" flag per workspace. Show a visual indicator when zoomed
   (e.g. a subtle "ZOOMED" label or border change).

3. TERMINAL SEARCH:
   Add Ctrl+Shift+F to trigger search.
   Call ghostty_surface_binding_action(surface, "search_forward", 14) on the focused terminal.
   This triggers ghostty's built-in search. Handle GHOSTTY_ACTION_START_SEARCH
   in action_cb. Also handle GHOSTTY_ACTION_SEARCH_TOTAL and GHOSTTY_ACTION_SEARCH_SELECTED
   if you want to show match count.

4. BROADCAST MODE:
   Add Ctrl+Shift+Enter to toggle broadcast mode per workspace.
   When enabled, every ghostty_surface_key and ghostty_surface_text call on any
   terminal in the workspace gets duplicated to ALL terminals in that workspace.
   Add a broadcast flag to Workspace struct.
   Show a visual indicator (e.g. red "BROADCAST" text in sidebar for that workspace).
   In GhosttyWidget::keyPressEvent, after calling ghostty_surface_key, if broadcast
   is enabled, iterate all other terminals in the workspace and send the same key.
   Same for ghostty_surface_text.

5. ACTIVITY INDICATOR:
   Handle GHOSTTY_ACTION_RENDER in action_cb. This fires when a terminal has new content.
   Use the target surface pointer to find which tab it belongs to.
   If that tab is NOT the currently visible tab in its pane, set a "has new output" flag.
   Show a small green dot next to inactive tab names that have new output.
   Clear the flag when the user switches to that tab.
   Use QTabBar::setTabTextColor or prepend a dot character.

6. PROGRESS BAR:
   Handle GHOSTTY_ACTION_PROGRESS_REPORT in action_cb.
   Store progress state and percentage per surface.
   Show a thin colored bar at the bottom of the pane tab:
   - Green for normal progress (0-100%)
   - Yellow for paused
   - Red for error
   - Pulsing for indeterminate
   Use QProgressBar or just paint a colored strip with setTabToolTip showing percentage.
   Clear on GHOSTTY_PROGRESS_STATE_REMOVE.

echo "Phase 1: Implementing terminal power features..."
After implementing, build: ${BUILD_CMD}
Fix any errors until build passes.

Return JSON: { summary: "...", filesChanged: ["path1", ...] }`}
        </Task>

        <Task id="review1" output={reviewResult} agent={reviewer} retries={2}>
          {`Review Phase 1 (Terminal Power Features) at ${PROJECT_DIR}.

Check:
1. Alt+Arrow moves focus between panes directionally
2. Alt+Arrow intercepted in GhosttyWidget before ghostty
3. Ctrl+Shift+Z toggles pane zoom, hides/shows other panes
4. Ctrl+Shift+F triggers ghostty search via binding_action
5. Ctrl+Shift+Enter toggles broadcast mode with visual indicator
6. Broadcast duplicates key/text input to all workspace terminals
7. GHOSTTY_ACTION_RENDER handled for activity indicator
8. Activity dot shown on inactive tabs with new output
9. GHOSTTY_ACTION_PROGRESS_REPORT handled
10. Progress shown visually on tabs
11. Build succeeds: ${BUILD_CMD}

If ALL pass: { approved: true, issues: [], summary: "Phase 1 looks good" }
If ANY issue: { approved: false, issues: ["..."], summary: "Found N issues" }`}
        </Task>
        </Sequence>
      </Ralph>

      {/* ═══════════════════════════════════════════════════════════════
          Phase 2: Productivity + Custom Keyboard Shortcuts
          ═══════════════════════════════════════════════════════════════ */}
      <Ralph
        until={ctx.latest(reviewResult, "review2")?.approved}
        maxIterations={3}
        onMaxReached="return-last"
      >
        <Sequence>
        <Task id="impl2" output={implResult} agent={coder} retries={2}>
          {`You are working on PrettyMux at ${PROJECT_DIR}.
Build with: ${BUILD_CMD}
Read src/qt/main.cpp to understand existing code.

${ctx.latest(reviewResult, "review2")?.issues?.length ? `\nPREVIOUS REVIEW FEEDBACK:\n${ctx.latest(reviewResult, "review2")!.issues.join("\n")}\n` : ""}

Implement PHASE 2: Productivity + Custom Keyboard Shortcuts

1. GIT STATUS IN SIDEBAR:
   Extend the existing git branch detection in updateWorkspaceCwd.
   After detecting branch, also run:
   - git status --porcelain (count lines for dirty file count)
   - git rev-list --count HEAD...@{upstream} 2>/dev/null (ahead/behind)
   Store dirty count and ahead/behind in Workspace struct.
   Show in refreshSidebarItem: "main +3 -1" or "main *5" (5 dirty files).
   Use colors: green for clean, yellow for dirty, red for conflicts.

2. PORT SCANNER:
   Add a QTimer that runs every 5 seconds (configurable).
   Read /proc/net/tcp and /proc/net/tcp6, parse hex port numbers for LISTEN state.
   Match ports to workspace PIDs using /proc/[pid]/fd/* -> socket:[inode].
   Actually simpler: just detect any new listening ports system-wide.
   Show detected ports in the sidebar under each workspace's CWD.
   When a new port appears, offer to open it in browser (add a clickable link).
   Allow disabling via PRETTYMUX_PORT_SCAN=0 env var.
   Keep it lightweight: cache results, skip if nothing changed.
   Store port list per workspace.

3. QUICK NOTES:
   Add Ctrl+Shift+Q to toggle a notes panel.
   Create a QTextEdit sidebar panel (or overlay) per workspace.
   Save notes content in session JSON.
   Restore on startup.
   Simple plain text, no formatting needed.
   Dark themed to match the app.

4. CUSTOM KEYBOARD SHORTCUTS:
   This is the big one. Modify the shortcut overlay (Ctrl+Shift+K):

   a) Store shortcuts in a QJsonObject loaded from ~/.config/prettymux/keybindings.json
      Default keybindings are hardcoded. User overrides are loaded from the file.

   b) In the shortcut overlay, make each shortcut row clickable/editable:
      - When user clicks on the key combination area, it enters "recording" mode
      - The key area shows "Press keys..." with a pulsing border
      - The next key combination the user presses gets captured and displayed
      - Show a Save button and a Cancel button
      - On Save: write to ~/.config/prettymux/keybindings.json
      - Add a "Reset to defaults" button at the bottom

   c) In keyPressEvent (both GhosttyWidget and PrettyMuxWindow), look up
      the action from the keybindings map instead of hardcoded switch cases.
      Create a ShortcutManager class/struct that:
      - Loads keybindings from JSON file
      - Falls back to defaults
      - Maps QKeySequence -> action string
      - Has matchShortcut(QKeyEvent*) -> optional<string> method

   d) Actions are strings like "workspace.new", "workspace.close", "pane.tab.new",
      "browser.toggle", "search.show", "shortcuts.show", etc.

echo "Phase 2: Implementing productivity features..."
After implementing, build: ${BUILD_CMD}
Fix any errors until build passes.

Return JSON: { summary: "...", filesChanged: ["path1", ...] }`}
        </Task>

        <Task id="review2" output={reviewResult} agent={reviewer} retries={2}>
          {`Review Phase 2 (Productivity + Custom Shortcuts) at ${PROJECT_DIR}.

Check:
1. Git status shows dirty count and ahead/behind in sidebar
2. Port scanner runs on timer, detects listening ports
3. Port scanner can be disabled via PRETTYMUX_PORT_SCAN=0
4. Port scanner doesn't hog CPU (timer based, caches results)
5. Quick notes panel toggles with Ctrl+Shift+Q
6. Notes saved/restored in session
7. Keybindings loaded from ~/.config/prettymux/keybindings.json
8. Shortcut overlay allows inline editing of key combos
9. "Recording" mode captures next key combination
10. Save/Cancel/Reset buttons work
11. ShortcutManager used in keyPressEvent instead of hardcoded switch
12. Build succeeds: ${BUILD_CMD}

If ALL pass: { approved: true, issues: [], summary: "Phase 2 looks good" }
If ANY issue: { approved: false, issues: ["..."], summary: "Found N issues" }`}
        </Task>
        </Sequence>
      </Ralph>

      {/* ═══════════════════════════════════════════════════════════════
          Phase 3: Quality of Life
          ═══════════════════════════════════════════════════════════════ */}
      <Ralph
        until={ctx.latest(reviewResult, "review3")?.approved}
        maxIterations={3}
        onMaxReached="return-last"
      >
        <Sequence>
        <Task id="impl3" output={implResult} agent={coder} retries={2}>
          {`You are working on PrettyMux at ${PROJECT_DIR}.
Build with: ${BUILD_CMD}
Read src/qt/main.cpp to understand existing code.

${ctx.latest(reviewResult, "review3")?.issues?.length ? `\nPREVIOUS REVIEW FEEDBACK:\n${ctx.latest(reviewResult, "review3")!.issues.join("\n")}\n` : ""}

Implement PHASE 3: Quality of Life

1. THEME SWITCHER:
   Add a theme system with at least 3 themes: Dark (current), Light, Monokai.
   Store theme as a struct with all colors (background, foreground, accent, etc.).
   Add Ctrl+Shift+comma to cycle themes (or show a theme picker in settings).
   Apply theme by updating QPalette and all widget stylesheets.
   Detect system preference via QStyleHints::colorScheme() on startup.
   Save selected theme in config.

2. DRAG AND DROP TABS:
   Enable drag and drop for terminal tabs between panes.
   When a tab is dragged from one PaneWidget to another:
   - Remove the GhosttyWidget from source pane
   - Add it to target pane
   - Update the terminals vector in both panes
   Use QTabBar::setMovable(true) (already set) for within-pane reorder.
   For cross-pane: implement dragEnterEvent, dragMoveEvent, dropEvent on PaneWidget.
   Use QMimeData to carry the source pane pointer and tab index.

3. COMMAND HISTORY SEARCH:
   Add Ctrl+Shift+H to open a command history search overlay.
   Read ~/.bash_history (or HISTFILE env var) and parse commands.
   Show a searchable list (same style as the search palette).
   When user selects a command, type it into the focused terminal
   via ghostty_surface_text.
   Cache the history file, refresh on overlay open.

4. PICTURE IN PICTURE:
   Add Ctrl+Shift+M to pop out the current browser tab as a floating window.
   Use Qt::WindowStaysOnTopHint | Qt::Tool | Qt::FramelessWindowHint for a clean float.
   Add a thin custom title bar with drag support (QWidget with mousePressEvent/mouseMoveEvent).
   Add resize handles on edges. Add a close button that returns the view to tabs.
   Use Qt::WA_TranslucentBackground for a modern look with rounded corners.
   On Wayland, Qt handles always-on-top natively via xdg-toplevel.
   Move the WebKit view from the tab into the PiP window.
   Store the original tab index so it can be restored.
   When PiP window closes (closeEvent), move the view back into browserTabs.
   Allow resizing the PiP window by dragging edges.
   Show a subtle shadow/border around the PiP window.

5. BROWSER ADDRESS BAR AUTOCOMPLETE:
   The browser address bar (QLineEdit urlBar) should have smart autocomplete.
   Keep a history of visited URLs in a QStringList (persisted in session JSON).
   Use QCompleter with the history list, set to case-insensitive popup mode.
   As user types, show matching URLs from history in a dropdown.
   Style the completer popup to match the dark theme.
   Also add typed URLs to history on Enter.
   Limit history to 500 entries, deduplicate.

6. RESIZE OVERLAY:
   When user resizes a splitter between panes, show a brief overlay
   with the pixel dimensions of each pane. Fade out after 1 second.
   Just a subtle QLabel that appears near the splitter handle.

echo "Phase 3: Implementing quality of life features..."
After implementing, build: ${BUILD_CMD}
Fix any errors until build passes.

Return JSON: { summary: "...", filesChanged: ["path1", ...] }`}
        </Task>

        <Task id="review3" output={reviewResult} agent={reviewer} retries={2}>
          {`Review Phase 3 (Quality of Life) at ${PROJECT_DIR}. FINAL REVIEW.

Check:
1. Theme switcher with at least 3 themes
2. Theme applied to all widgets (palette + stylesheets)
3. Theme saved in config
4. Tab drag and drop works within panes (reorder)
5. Command history reads ~/.bash_history
6. History search overlay matches search palette style
7. Selected command typed into terminal
8. Browser PiP creates floating always-on-top window
9. PiP window returns view to tabs on close
10. Browser address bar has QCompleter with URL history
11. URL history persisted in session, deduplicated, max 500
12. Build succeeds: ${BUILD_CMD}

If ALL pass: { approved: true, issues: [], summary: "Phase 3 complete" }
If ANY issue: { approved: false, issues: ["..."], summary: "Found N issues" }`}
        </Task>
        </Sequence>
      </Ralph>

    </Sequence>
  </Workflow>
));
