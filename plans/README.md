# prettymux — improvement plans index

Audit run: senior-advisor `/improve` (standard depth) against **HEAD `b2d3cd0`** (Release 0.2.42).
Status: **all 7 findings implemented, codex-reviewed, and pushed** (`a464c43..91a5430`). Each was committed, reviewed by `/codex-fix`, and review findings fixed; the whole batch passed a final `/codex-fix`. 13/13 tests green throughout.

Repo: native Linux terminal multiplexer, C17 + GTK4 + libadwaita + libghostty, meson/ninja, ~36K LOC in `src/gtk/`.

**Verification commands** (every future plan must use these as gates):
- Build: `ninja -C builddir`
- Tests: `meson test -C builddir` (13 tests; all green at audit time)
- (Re-configure if needed: `meson setup builddir src/gtk -Dghostty_dir=<ghostty> --buildtype=release`)

---

## Findings (vetted, ordered by leverage)

Leverage = impact ÷ effort, discounted by confidence and fix-risk. All evidence re-read by hand during vetting; subagent over-reporting corrected (see "Considered and rejected").

| # | Finding | Category | Impact | Effort | Risk | Conf | Status |
|---|---------|----------|--------|--------|------|------|--------|
| 1 | CLAUDE.md describes a Bun/React/Node project; this is a C/GTK/meson app. Vestigial tracked `index.ts`, `tsconfig.json` | DX | Every agent session mis-instructed ("Default to using Bun") | S | LOW | HIGH | ✅ DONE `e8521a3` (+ `91a5430` removed stale `skills/development.md`) |
| 2 | IPC socket unhardened: predictable `/tmp/prettymux-<id>.sock`, no peer-cred, umask-dependent connectability; `exec` injects arbitrary text into focused terminal | Security | Local same-host command execution as the user; `/tmp` squatting/TOCTOU | M | MED | HIGH | ✅ DONE `51ce5fa` + `9caf780` (moved to `$XDG_RUNTIME_DIR`, all 4 path-builders + scans) |
| 3 | Four dead static functions in `workspace.c` | Tech debt | Build warnings; dead DnD/rename remnants | S | LOW | HIGH | ✅ DONE `8463ede` |
| 4 | Deprecated `gtk_gl_area_set_use_es` (2 sites) | Deps | Deprecation warnings; future GTK removal risk | S | LOW | HIGH | ✅ DONE `f3ed9cc` |
| 5 | Session JSON parsing trusts member types; parse error swallowed | Correctness | Corrupt/edited session → `g_critical` spam + silently dropped state (NOT a crash; glib-guarded) | S | LOW | HIGH | ✅ DONE `7765f3a` + `54dca9d` |
| 6 | Socket IPC protocol undocumented (README markets "agent-friendly notifications") | Docs | Agents/scripts must read C source to use the differentiator | M | LOW | HIGH | ✅ DONE `63f4c68` + `ea326bd` (`docs/socket-protocol.md`) |
| 7 | Socket command dispatch under-tested (error paths + several commands) | Tests | Agent-facing IPC regressions land silently | M | LOW | HIGH | ✅ DONE `798e472` |

### Evidence per finding

- **#1** — `CLAUDE.md` (entire file is Bun.serve / React / `bun:sqlite` / `bun test` guidance, none applicable). `index.ts` (30 bytes, `console.log("Hello via Bun!")`) and `tsconfig.json` tracked in git but unused. `package.json` + `bun.lock` are gitignored (local only; `package.json` pulls `smithers-orchestrator`+`zod`, unused by the C app). Fix: replace CLAUDE.md with accurate C/GTK/meson guidance (build/test commands, module map: `workspace.c`, `session.c`, `socket_commands.c`, `app_actions.c`, `ghostty_terminal.c`, `workspace_strip.c`); delete or relocate `index.ts`/`tsconfig.json`.
- **#2** — `socket_server.c:60` builds `/tmp/prettymux-%s.sock`; `:421-432` check-then-`g_unlink`-then-bind TOCTOU; no `SO_PEERCRED`/`g_socket_get_credentials` anywhere in the tree; `socket_commands.c:1000-1020` `exec` → `ghostty_surface_text(surface, cmd...)` + `"\n"`. Fix: move socket to `$XDG_RUNTIME_DIR` (0700, per-user) with `/tmp` fallback; optionally verify peer UID in `on_incoming`. **Blast radius / main risk:** the path is also built/read by the clients — `prettymux-open.c`, `prettymux_agent_cli.c`, `socket_server.c:370` (`build_socket_path_for_instance`), and shell-integration env (`PRETTYMUX_SOCKET`). All path-builders must change together, and back-compat for already-running instances considered. Fold the `open.url` scheme-validation (`socket_commands.c:240`) in here as defense-in-depth.
- **#3** — `workspace.c:3081` `setup_notebook_drop_target`, `:3058` `setup_tab_label_dnd`, `:2553` `on_rename_entry_focus_leave`, `:2311` `start_workspace_rename_popover`. Grep shows only definition/forward-decl, no call sites; build emits `-Wunused-function` for all four. Delete functions + any forward decls; rebuild must stay green and warning-free for these.
- **#4** — `ghostty_terminal.c:1167`, `main.c:76` `gtk_gl_area_set_use_es(area, FALSE)` → `gtk_gl_area_set_allowed_apis(area, GDK_GL_API_GL)`. Confirm meson GTK min version supports it (≥4.6; current dep is gtk4 unversioned — may need a version floor bump).
- **#5** — `session.c:809-811` `json_object_has_member("columns")` then `json_object_get_array_member` (returns NULL on wrong type) then `json_array_get_length` (glib `g_return_val_if_fail` → 0 + critical, no crash); ~4 similar sites (logoCache, workspaces, panes, tabs). `session.c:1457` `json_parser_load_from_file(parser, path, NULL)` swallows parse errors. Fix: guard array-member access with `JSON_NODE_HOLDS_ARRAY`/NULL check; capture+log the parse `GError`. Defensive only; session files are self-written so low real-world frequency.
- **#6** — README §automation shows examples only; full protocol (40+ commands) lives in `socket_commands.c` and `prettymux-open.c:69-99` `--help`. Fix: generate `docs/socket-protocol.md` (command, required fields, example req/resp) + document the workspace.status.* agent-notification feature; link from README.
- **#7** — `test_socket_commands_status.c` covers workspace.status set/clear/list only. Untested: error paths (bad index/missing field), and commands `port.report`, `terminal.register`, `open.url`, `pane.read_text`. Extend that test file (it's the established pattern). **This is the down payment for any future `workspace.c` decomposition** (see Direction).

---

## Considered and rejected (do not re-audit)

- **PERF: per-frame `gtk_widget_set_size_request`** (`workspace_strip.c:207-214`) — GTK short-circuits unchanged values; the tick callback stops when not animating. Necessary work during animation. Not worth doing.
- **PERF: focused-pane linear scan** (`workspace.c` `workspace_get_focused_pane` → `workspace_has_pane`) — O(panes) per keypress; panes are single-digit and keypresses are human-speed. Micro-optimization.
- **PERF: redundant `gtk_widget_get_width`** (`workspace_strip.c` pan/maximize) — micro.
- **CORRECT: layout-rebuild "race"** (`workspace_layout.c:87-95`) — single-threaded GTK main loop; no main-loop yield between clearing and rebuilding state. False positive.
- **CORRECT: timeout ref-leak on cancel** (`workspace.c:2116-2117` `g_timeout_add` + `g_object_ref`) — only leaks if the source is cancelled before firing (rare, shutdown-only); MED confidence, very low impact. Not worth a plan.
- **`open.url` scheme validation** — glib already blocks dangerous schemes; folded into #2 as defense-in-depth rather than its own finding.

---

## Direction (maintainer's call — options, not ranked vs. bugs)

- **Decompose `workspace.c` (6,239 lines)** — mixes pane lifecycle, layout branching, rename UI, DnD, session serialization. Real god-object, but L-effort / HIGH-risk. Sequencing: **characterization tests first** (#7 is a start), then split along seams (`workspace_session.c`, `workspace_dnd.c`, `workspace_ui.c`). Don't refactor blind.
- **IPC surface symmetry** — `workspace.import` without `export`; `status.set`/`list` without `get`; `tab.new` without `tab.get`. Additive/back-compat, but confirm intent first (MED confidence it's an oversight). Pairs with #6/#7.
- **Classic vs. strip layout strategy** — strip is now default and gets all recent work/tests; classic is dual-maintained and lightly tested. Decide: invest in parity, or deprecate classic. Drives how much layout-branching debt is worth paying.

---

## Recommended execution order

1. **#1** (CLAUDE.md + vestigial files) — highest leverage, unblocks correct agent behavior for all later work.
2. **#3**, **#4** (dead code, deprecated API) — trivial, clean verification, removes build warnings.
3. **#2** (socket hardening) — highest real security value; touch all path-builders together.
4. **#5** (session JSON hardening).
5. **#6** (protocol docs), then **#7** (socket tests).

**Dependency:** #7's characterization tests should precede any `workspace.c` decomposition (Direction item 1).

When writing plan files: number them `001-<slug>.md` … using the template at `/home/pe/.claude/skills/improve/references/plan-template.md`, keep this index's numbering, and stamp each plan with the commit it was written against.
