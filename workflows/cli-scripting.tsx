/**
 * Smithers workflow: PrettyMux CLI, Socket API, Scripting & Polish
 *
 * Extends the Qt6 C++ terminal multiplexer with a full socket API,
 * Python CLI, config system, session persistence, scripting, and
 * keyboard shortcuts.
 *
 * Usage:
 *   bunx smithers run workflows/cli-scripting.tsx
 *
 * Phase 1: Socket API + Python CLI
 * Phase 2: Config system + Session persistence
 * Phase 3: Python scripting module + JS scripting via QWebChannel
 * Phase 4: Keyboard shortcuts, shell integration, CI
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
  impl4: implResult,
  review4: reviewResult,
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
  <Workflow name="prettymux-cli-scripting">
    <Sequence>

      {/* ═══════════════════════════════════════════════════════════════
          Phase 1: Socket API + Python CLI
          ═══════════════════════════════════════════════════════════════ */}
      <Ralph
        until={ctx.latest(reviewResult, "review1")?.approved}
        maxIterations={3}
        onMaxReached="return-last"
      >
        <Sequence>
        <Task id="impl1" output={implResult} agent={coder} retries={2}>
          {`You are working on PrettyMux at ${PROJECT_DIR}.

This is a Qt6 C++ terminal multiplexer. The main source is src/qt/main.cpp.
Build with: ${BUILD_CMD}

The app ALREADY has a basic QLocalServer socket that handles browser.open
commands. You need to EXTEND it (not rewrite) to handle all commands.

IMPORTANT: Read src/qt/main.cpp first to understand the existing code structure.
The app uses QLocalServer, has Workspace structs with PaneWidgets, QWebEngineView
browser tabs, and GhosttyWidget terminals.

${ctx.latest(reviewResult, "review1")?.issues?.length ? `\nPREVIOUS REVIEW FEEDBACK:\n${ctx.latest(reviewResult, "review1")!.issues.join("\n")}\n` : ""}

Implement PHASE 1: Socket API + Python CLI

1. EXTEND the existing QLocalServer handler in main.cpp to support these commands:
   JSON protocol: {"command": "...", "args": {...}} -> {"success": true, "data": ...}

   Workspace commands:
   - workspace.new { name? } -> { id, name }
   - workspace.list -> [{ id, name, cwd, gitBranch, active }]
   - workspace.select { index } -> void
   - workspace.next / workspace.previous -> void
   - workspace.close { index? } -> void
   - workspace.rename { index?, name } -> void

   Pane commands:
   - pane.split { direction: "right"|"down" } -> void
   - pane.close -> void
   - pane.list -> [{ id, tabs, activeTab }]
   - pane.tab.new -> void
   - pane.tab.close { index? } -> void

   Browser commands:
   - browser.open { url } -> void (already exists, keep it)
   - browser.navigate { url } -> void
   - browser.close { index? } -> void
   - browser.list -> [{ index, url, title }]

   Other:
   - notify { title, body } -> void
   - send-keys { keys } -> void (send to focused terminal)
   - current -> { workspace: {...}, pane: {...} }

   Each command handler should call the appropriate methods on PrettyMuxWindow.
   Use Q_INVOKABLE on new methods so they can be called via QMetaObject::invokeMethod
   from the socket thread if needed.

2. Create src/cli/prettymux — a Python CLI script (NOT a module, just a script):
   #!/usr/bin/env python3

   - Connects to Unix socket at PRETTYMUX_SOCKET env var or /tmp/prettymux-*.sock
   - Auto-discovers socket by globbing /tmp/prettymux-*.sock if env not set
   - Subcommands via argparse:
     prettymux workspace new [name]
     prettymux workspace list [--json]
     prettymux workspace select <index>
     prettymux workspace next / previous
     prettymux workspace close [index]
     prettymux workspace rename <name>
     prettymux pane split-right / split-down
     prettymux pane close
     prettymux pane list [--json]
     prettymux pane tab new / close
     prettymux browser open <url>
     prettymux browser navigate <url>
     prettymux browser close [index]
     prettymux browser list [--json]
     prettymux notify <title> [body]
     prettymux send-keys <keys>
     prettymux current [--json]
   - Pretty-print tables for list commands, JSON with --json flag
   - chmod +x the script

echo "Phase 1: Extending socket server..."
echo "Phase 1: Creating Python CLI..."

After implementing, build: ${BUILD_CMD}
Fix any errors until build passes.
Also test: python3 src/cli/prettymux --help

Return JSON: { summary: "...", filesChanged: ["path1", ...] }`}
        </Task>

        <Task id="review1" output={reviewResult} agent={reviewer} retries={2}>
          {`Review Phase 1 (Socket API + Python CLI) at ${PROJECT_DIR}.

Check:

1. src/qt/main.cpp socket handler extended with all commands listed above
2. Each command properly calls PrettyMuxWindow methods
3. JSON protocol is consistent: request has "command" + "args", response has "success" + "data"
4. src/cli/prettymux exists, is executable (chmod +x), has shebang
5. CLI connects to socket and sends/receives JSON
6. CLI has all subcommands with argparse
7. CLI auto-discovers socket via glob
8. --json flag works for list commands
9. Existing functionality not broken (browser.open still works)

Build: ${BUILD_CMD}
Test: python3 src/cli/prettymux --help

If ALL checks pass AND build succeeds: { approved: true, issues: [], summary: "Phase 1 looks good" }
If ANY issue: { approved: false, issues: ["..."], summary: "Found N issues" }`}
        </Task>
        </Sequence>
      </Ralph>

      {/* ═══════════════════════════════════════════════════════════════
          Phase 2: Config System + Session Persistence
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

Read src/qt/main.cpp to understand the existing code.

${ctx.latest(reviewResult, "review2")?.issues?.length ? `\nPREVIOUS REVIEW FEEDBACK:\n${ctx.latest(reviewResult, "review2")!.issues.join("\n")}\n` : ""}

Implement PHASE 2: Config System + Session Persistence

1. Add a config system to main.cpp:
   - Read ~/.config/prettymux/config on startup (key = value format)
   - Settings:
     sidebar-width = 200 (initial sidebar width)
     sidebar-visible = true
     browser-visible = true
     default-url = file:///path/to/welcome.html
     notification-desktop = true
     notification-sound = false
     restore-session = true
     socket-path = /tmp/prettymux.sock
   - Use QFileSystemWatcher to watch the config file for changes
   - Apply settings on load and on change
   - Create a ConfigManager class or struct in main.cpp

2. Add session persistence:
   - On exit (closeEvent): save to ~/.prettymux/sessions/last.json
   - Save: window geometry, workspace names, number of panes per workspace,
     browser tab URLs, active workspace index, sidebar visibility
   - On startup: if restore-session=true and last.json exists, restore layout
   - Auto-save every 30 seconds via QTimer
   - Use QJsonDocument for serialization
   - Create dirs if they don't exist (QDir::mkpath)

3. Add a "restore-session" socket command so the CLI can trigger restore

echo "Phase 2: Adding config system..."
echo "Phase 2: Adding session persistence..."

After implementing, build: ${BUILD_CMD}
Fix any errors until build passes.

Return JSON: { summary: "...", filesChanged: ["path1", ...] }`}
        </Task>

        <Task id="review2" output={reviewResult} agent={reviewer} retries={2}>
          {`Review Phase 2 (Config + Session Persistence) at ${PROJECT_DIR}.

Check:

1. Config reads from ~/.config/prettymux/config (key = value)
2. QFileSystemWatcher watches the config file
3. Settings are applied (sidebar width, visibility, etc.)
4. Session saves to ~/.prettymux/sessions/last.json on exit
5. Session restores on startup when restore-session=true
6. Auto-save every 30 seconds via QTimer
7. QJsonDocument used for JSON serialization
8. Directories created if missing
9. Existing functionality not broken

Build: ${BUILD_CMD}

If ALL checks pass AND build succeeds: { approved: true, issues: [], summary: "Phase 2 looks good" }
If ANY issue: { approved: false, issues: ["..."], summary: "Found N issues" }`}
        </Task>
        </Sequence>
      </Ralph>

      {/* ═══════════════════════════════════════════════════════════════
          Phase 3: Python Scripting + JS Scripting via QWebChannel
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

Read src/qt/main.cpp to understand the existing code.
The socket API from Phase 1 is already implemented.

${ctx.latest(reviewResult, "review3")?.issues?.length ? `\nPREVIOUS REVIEW FEEDBACK:\n${ctx.latest(reviewResult, "review3")!.issues.join("\n")}\n` : ""}

Implement PHASE 3: Python Scripting Module + JS Scripting

1. Create src/scripting/prettymux.py — a Python module that wraps the socket API:
   import prettymux

   # Auto-discovers socket
   pmux = prettymux.connect()

   # Workspace operations
   pmux.workspace.new("dev")
   pmux.workspace.list()  # returns list of dicts
   pmux.workspace.select(0)
   pmux.workspace.next()
   pmux.workspace.close()

   # Pane operations
   pmux.pane.split_right()
   pmux.pane.split_down()
   pmux.pane.tab.new()
   pmux.pane.list()

   # Browser
   pmux.browser.open("https://google.com")
   pmux.browser.list()

   # Other
   pmux.notify("title", "body")
   pmux.send_keys("ls -la\\n")
   pmux.current()

   The module should:
   - Use socket module to connect to Unix socket
   - Send JSON commands, receive JSON responses
   - Auto-discover socket path from PRETTYMUX_SOCKET env or glob
   - Raise exceptions on errors
   - Be a single file, no deps beyond stdlib

2. Add JavaScript scripting via QWebChannel in main.cpp:
   - Add QWebChannel to CMakeLists.txt: find_package(Qt6 REQUIRED COMPONENTS WebChannel)
     and target_link_libraries(... Qt6::WebChannel)
   - Create a PrettyMuxBridge QObject class with Q_INVOKABLE methods:
     - workspaceNew(name), workspaceList(), workspaceSelect(index)
     - paneSplitRight(), paneSplitDown(), paneList()
     - browserOpen(url), browserList()
     - notify(title, body), sendKeys(keys)
   - Register it with QWebChannel on each QWebEngineView page
   - This exposes window.prettymux in all browser tabs:
     window.prettymux.workspaceNew("dev")
     window.prettymux.browserOpen("https://example.com")

echo "Phase 3: Creating Python scripting module..."
echo "Phase 3: Adding QWebChannel JS bridge..."

After implementing, build: ${BUILD_CMD}
Fix any errors until build passes.
Test: python3 -c "import sys; sys.path.insert(0, 'src/scripting'); import prettymux; print('OK')"

Return JSON: { summary: "...", filesChanged: ["path1", ...] }`}
        </Task>

        <Task id="review3" output={reviewResult} agent={reviewer} retries={2}>
          {`Review Phase 3 (Scripting) at ${PROJECT_DIR}.

Check:

1. src/scripting/prettymux.py exists with socket-based API wrapper
2. Module auto-discovers socket path
3. Module has workspace, pane, browser, notify, send_keys methods
4. Module is a single file with no external deps
5. CMakeLists.txt includes Qt6::WebChannel
6. PrettyMuxBridge QObject class exists with Q_INVOKABLE methods
7. QWebChannel registered on browser pages
8. window.prettymux available in browser JS context
9. Build succeeds with WebChannel

Build: ${BUILD_CMD}
Test: python3 -c "import sys; sys.path.insert(0, 'src/scripting'); import prettymux; print('OK')"

If ALL checks pass AND build succeeds: { approved: true, issues: [], summary: "Phase 3 looks good" }
If ANY issue: { approved: false, issues: ["..."], summary: "Found N issues" }`}
        </Task>
        </Sequence>
      </Ralph>

      {/* ═══════════════════════════════════════════════════════════════
          Phase 4: Keyboard Shortcuts, Shell Integration, CI
          ═══════════════════════════════════════════════════════════════ */}
      <Ralph
        until={ctx.latest(reviewResult, "review4")?.approved}
        maxIterations={3}
        onMaxReached="return-last"
      >
        <Sequence>
        <Task id="impl4" output={implResult} agent={coder} retries={2}>
          {`You are working on PrettyMux at ${PROJECT_DIR}.
Build with: ${BUILD_CMD}

Read src/qt/main.cpp to understand the existing keyboard shortcuts.

${ctx.latest(reviewResult, "review4")?.issues?.length ? `\nPREVIOUS REVIEW FEEDBACK:\n${ctx.latest(reviewResult, "review4")!.issues.join("\n")}\n` : ""}

Implement PHASE 4: Keyboard Shortcuts, Shell Integration, CI

1. Add missing keyboard shortcuts to PrettyMuxWindow::keyPressEvent:
   - Ctrl+Shift+W: close current workspace
   - Alt+Arrow keys: focus pane in that direction (find nearest pane widget)
   - F11: toggle fullscreen (showFullScreen / showNormal)
   - Ctrl+Shift+]: next workspace
   - Ctrl+Shift+[: previous workspace
   - Ctrl+Shift+N: new workspace (keep Ctrl+N too)
   - Ctrl+W: close current browser tab (when browser focused)
   - Ctrl+L: focus browser address bar (when browser focused)

   IMPORTANT: Intercept Ctrl+Shift+W and Ctrl+Shift+]/[ in GhosttyWidget
   before ghostty consumes them, same pattern as Ctrl+Shift+T.

2. Create shell integration scripts:

   src/shell-integration/prettymux.bash:
   - Check if PRETTYMUX=1, if not return
   - Override open/xdg-open to route URLs to embedded browser (like existing injection)
   - Add prettymux() function that calls the CLI
   - Source automatically via BASH_ENV

   src/shell-integration/prettymux.zsh:
   - Same as bash but for zsh
   - Use chpwd hook for directory tracking

   src/shell-integration/prettymux.fish:
   - Same for fish shell
   - Use --on-variable PWD

3. Create .github/workflows/build.yml:
   name: Build PrettyMux
   on: [push, pull_request]
   jobs:
     build-ubuntu:
       runs-on: ubuntu-latest
       steps:
         - checkout
         - Install deps: sudo apt install qt6-base-dev qt6-webengine-dev libgl-dev cmake
         - Build ghostty: cd ghostty && zig build -Dapp-runtime=none -Doptimize=ReleaseFast
         - Build prettymux: cd src/qt/build && cmake .. -DCMAKE_BUILD_TYPE=Release && make
         - Upload artifact: prettymux binary

4. Update welcome.html to show all current keyboard shortcuts

echo "Phase 4: Adding keyboard shortcuts..."
echo "Phase 4: Creating shell integration..."
echo "Phase 4: Creating GitHub Actions CI..."

After implementing, build: ${BUILD_CMD}
Fix any errors until build passes.

Return JSON: { summary: "...", filesChanged: ["path1", ...] }`}
        </Task>

        <Task id="review4" output={reviewResult} agent={reviewer} retries={2}>
          {`Review Phase 4 (Shortcuts, Shell, CI) at ${PROJECT_DIR}. FINAL REVIEW.

Check:

1. Ctrl+Shift+W closes workspace
2. Alt+Arrow focuses pane directionally
3. F11 toggles fullscreen
4. Ctrl+Shift+]/[ cycles workspaces
5. Ctrl+W closes browser tab when browser focused
6. Shortcuts intercepted in GhosttyWidget before ghostty
7. src/shell-integration/prettymux.bash exists with PRETTYMUX check
8. src/shell-integration/prettymux.zsh exists
9. src/shell-integration/prettymux.fish exists
10. .github/workflows/build.yml exists with ubuntu job
11. welcome.html updated with shortcuts
12. Build succeeds

Build: ${BUILD_CMD}

If ALL checks pass AND build succeeds: { approved: true, issues: [], summary: "Phase 4 complete" }
If ANY issue: { approved: false, issues: ["..."], summary: "Found N issues" }`}
        </Task>
        </Sequence>
      </Ralph>

    </Sequence>
  </Workflow>
));
