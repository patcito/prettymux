// smithers-display-name: PrettyMux Strip Layout
/** @jsxImportSource smthrs */
import { createSmithers } from "smthrs";
import { z } from "zod/v4";
import { agents } from "~/agents";
import { ValidationLoop, implementOutputSchema, validateOutputSchema } from "~/components/ValidationLoop";
import { reviewOutputSchema } from "~/components/Review";

const inputSchema = z.object({
  prompt: z.string().default(
    "Implement the PrettyMux strip-layout and sidebar roadmap from .smithers/tickets/prettymux-strip-layout-plan.md.",
  ),
});

const { Workflow, Sequence, smithers } = createSmithers({
  input: inputSchema,
  implement: implementOutputSchema,
  validate: validateOutputSchema,
  review: reviewOutputSchema,
});

function phaseState(ctx: any, idPrefix: string) {
  const allReviews = ctx.outputs.review ?? [];
  const reviews = allReviews.filter((r: any) => {
    const nid: string | undefined = r?.__nodeId ?? r?.nodeId;
    return nid ? nid.startsWith(`${idPrefix}:`) : true;
  });

  const anyApproved = reviews.length > 0 && reviews.some((r: any) => r.approved === true);
  const done = anyApproved;

  const parts: string[] = [];
  for (const review of reviews) {
    if (review.approved === false) {
      parts.push(`REVIEWER REJECTED:\n${review.feedback}`);
      for (const issue of review.issues ?? []) {
        parts.push(
          `  [${issue.severity}] ${issue.title}: ${issue.description}${issue.file ? ` (${issue.file})` : ""}`,
        );
      }
    }
  }
  return { done, feedback: parts.length ? parts.join("\n\n") : null };
}

function preflightState(ctx: any, idPrefix: string) {
  const allReviews = ctx.outputs.review ?? [];
  const reviews = allReviews.filter((r: any) => {
    const nid: string | undefined = r?.__nodeId ?? r?.nodeId;
    return nid ? nid.startsWith(`${idPrefix}:`) : true;
  });
  const anyApproved = reviews.some((r: any) => r.approved === true);
  const anyRejected = reviews.some((r: any) => r.approved === false);
  return {
    done: anyApproved,
    failed: !anyApproved && anyRejected,
    feedback: reviews
      .filter((r: any) => r.approved === false)
      .map((r: any) => `REVIEWER REJECTED:\n${r.feedback}`)
      .join("\n\n") || null,
  };
}

const sharedContext = `
SOURCE PLAN:
- Read .smithers/tickets/prettymux-strip-layout-plan.md before editing.

CROSS-PHASE INVARIANTS:
- Keep the current classic GtkPaned layout fully working.
- Add strip layout as a second backend; do not rewrite the app around it.
- Keep Ctrl+Shift+Z simple:
  - classic mode keeps its current behavior
  - strip mode uses maximize/unmaximize of the active column
- Preserve existing session restore behavior for classic layouts.
- Do not add Yoga in the first strip-layout implementation.
- Reuse ideas from Séance where they are clearly valuable.
- Séance is MIT-licensed, so direct code reuse is allowed where it makes sense, but copied code must retain attribution/license notice where materially reused.
- Prefer focused, reviewable diffs over broad speculative rewrites.
- For user-visible behavior, prefer features to be reachable through prettymux-open when that is a sensible automation boundary.

CROSS-PHASE VERIFICATION PROTOCOL:
- For user-visible phases, run PrettyMux inside a Docker container as part of implementation validation.
- The container verification path must support Wayland.
- For local verification, prefer sharing the host Wayland session by mounting the existing Wayland socket/runtime dir into the container.
- Use a headless Weston-style Wayland compositor only as a fallback for isolated CI or when host Wayland sharing is unavailable.
- Exercise new user-visible behavior through prettymux-open wherever that is a sensible automation boundary.
- If a new feature is reasonably scriptable but is not yet accessible through prettymux-open, implement the required prettymux-open / socket / action plumbing as part of the phase.
- In your summary, explicitly list:
  - the Docker command or script used
  - the Wayland/container setup used
  - the prettymux-open command(s) used to exercise the feature
- The verification is only valid if PrettyMux actually launches and prettymux-open talks to a live instance.
- REVIEWER side:
  - for user-visible phases, treat missing Docker + Wayland smoke verification as a real issue
  - for user-visible phases, treat missing prettymux-open coverage for a reasonably scriptable new feature as a real issue unless the implementer gives a concrete constraint-driven reason
  - treat CLI-only prettymux-open parsing checks against no running instance as insufficient
  - prefer concrete verification evidence over claims

CROSS-AGENT NEGOTIATION PROTOCOL (applies on ALL phases, on any iteration after the first — i.e. whenever the reviewer is re-reviewing code written in response to a prior rejection):

  IMPLEMENTER side — when you CANNOT apply a reviewer rejection verbatim due to a real technical constraint:
    * Do NOT silently diverge, half-implement, or quietly rephrase the spec to match what you did.
    * In your summary field, explicitly include a "CONSTRAINT-DRIVEN DEVIATION" section with four parts:
        1. Quote the rejection item(s) you could not implement as-stated.
        2. Explain the underlying constraint concretely, with a reference where possible.
        3. Propose the best alternative you can implement that preserves the INTENT of the requirement.
        4. Describe what you actually implemented under that alternative.
    * If you CAN implement every rejection verbatim, do so and skip this section entirely.

  REVIEWER side — on iteration >= 1, when re-reviewing code after a prior rejection:
    * Read the implementer's summary before inspecting the diff.
    * If the implementer cites a real technical constraint and proposes an alternative that preserves the spirit of the original requirement, accept it.
    * Do NOT re-reject the same item unless the implementer ignored it or the proposed alternative breaks a core invariant from the plan.
`;

export default smithers((ctx) => {
  const p0 = preflightState(ctx, "phase0");
  const p1 = phaseState(ctx, "phase1");
  const p1b = phaseState(ctx, "phase1b");
  const p2 = phaseState(ctx, "phase2");
  const p3 = phaseState(ctx, "phase3");
  const p3b = phaseState(ctx, "phase3b");
  const p4 = phaseState(ctx, "phase4");
  const p5 = phaseState(ctx, "phase5");
  const p5b = phaseState(ctx, "phase5b");
  const p6 = phaseState(ctx, "phase6");
  const p6b = phaseState(ctx, "phase6b");
  const p7 = phaseState(ctx, "phase7");
  const p8 = phaseState(ctx, "phase8");
  const p8b = phaseState(ctx, "phase8b");
  const p9 = phaseState(ctx, "phase9");
  const p10 = phaseState(ctx, "phase10");
  const p10b = phaseState(ctx, "phase10b");

  return (
    <Workflow name="prettymux-strip-layout">
      <Sequence>
        <Sequence>
          <ValidationLoop
            idPrefix="phase0"
            prompt={`${sharedContext}

PHASE 0: Docker + Wayland preflight

Own these files:
- packaging/containers/debian-bookworm.Dockerfile
- packaging/containers/verify-strip-layout.sh

Before any feature work, prove the verification environment itself is real.

Required outcomes:
- the verification path prefers host Wayland socket sharing for local runs
- PrettyMux actually launches in the container against a live Wayland session
- prettymux-open reaches the live instance and succeeds with --list-workspaces
- if host Wayland is unavailable, the script has a concrete fallback path

This phase gets one iteration only. If review rejects it, the rest of the workflow must not proceed.

Run the verification and return { summary, filesChanged, allTestsPassing } with the exact Docker/script command used.`}
            implementAgents={agents.smartTool}
            reviewAgents={agents.reviewer}
            feedback={p0.feedback}
            done={p0.done}
            maxIterations={1}
          />
        </Sequence>

        {p0.failed ? null : (
          <>
        <Sequence>
          <ValidationLoop
            idPrefix="phase1"
            prompt={`${sharedContext}

PHASE 1: Sidebar card UI

Own these files:
- src/gtk/sidebar_ui.c
- src/gtk/sidebar_ui.h
- src/gtk/theme.c

Implement the visible workspace-card UI upgrade inspired by Séance, but adapted to PrettyMux.

Required outcomes:
- bold title row for each workspace
- smaller metadata rows below it
- optional attention badge and compact structure hints
- preserve inline rename behavior, row selection, drag/reorder, and close handling
- the visual card treatment must be obvious in sidebar_ui.c and theme.c

Add or refactor UI helpers with clear ownership:
- sidebar_ui_build_workspace_card() or an equivalently clear card builder

Out of scope for this phase unless strictly required to render the card:
- async branch-safety fixes
- general workspace metadata correctness cleanup
- broader workspace.c refactors

Run a build and any directly relevant tests at the end. Return { summary, filesChanged, allTestsPassing }.`}
            implementAgents={agents.smartTool}
            reviewAgents={agents.reviewer}
            feedback={p1.feedback}
            done={p1.done}
            maxIterations={3}
          />
        </Sequence>

        <Sequence>
          <ValidationLoop
            idPrefix="phase1b"
            prompt={`${sharedContext}

PHASE 1B: Sidebar data correctness

Own these files:
- src/gtk/workspace.c
- src/gtk/workspace.h
- src/gtk/sidebar_ui.c
- src/gtk/sidebar_ui.h

Implement the stable workspace-summary data needed by the new sidebar cards.

Required outcomes:
- stable primary path derived from the first tab of the first pane unless you can justify and document a better stable rule
- stable primary branch behavior for sidebar summaries
- clean helper ownership for sidebar summary data

Add or refactor helpers with clear ownership:
- workspace_get_sidebar_primary_cwd()
- workspace_get_sidebar_primary_branch()
- workspace_get_sidebar_status_summary()
- workspace_get_sidebar_column_count()
- workspace_get_sidebar_attention_state()

If async branch/cwd correctness fixes are needed to make the sidebar data stable, they belong in this phase rather than phase 1.

Run a build and any directly relevant tests at the end. Return { summary, filesChanged, allTestsPassing }.`}
            implementAgents={agents.smartTool}
            reviewAgents={agents.reviewer}
            feedback={p1b.feedback}
            done={p1b.done}
            maxIterations={3}
          />
        </Sequence>

        <Sequence>
          <ValidationLoop
            idPrefix="phase2"
            prompt={`${sharedContext}

PHASE 2: Explicit layout backend split

Own these files:
- src/gtk/workspace.h
- src/gtk/workspace.c
- src/gtk/workspace_layout.h
- src/gtk/workspace_layout.c
- src/gtk/workspace_strip.h
- src/gtk/workspace_strip.c

Introduce a clean layout backend split without changing user-visible behavior yet.

Required types/state:
- WorkspaceLayoutMode
- WorkspaceStripState
- WorkspaceColumn

Required API surface:
- workspace_get_layout_mode()
- workspace_set_layout_mode()
- workspace_layout_create_root()
- workspace_layout_focus_primary()
- workspace_layout_toggle_zoom_current()

Requirements:
- classic layout must continue using the existing GtkPaned tree
- the new strip backend scaffolding must compile cleanly
- route layout-sensitive calls through backend helpers instead of direct paned assumptions where practical
- avoid speculative feature work beyond the backend split

Run a build at the end. Return { summary, filesChanged, allTestsPassing }.`}
            implementAgents={agents.smartTool}
            reviewAgents={agents.reviewer}
            feedback={p2.feedback}
            done={p2.done}
            maxIterations={3}
          />
        </Sequence>

        <Sequence>
          <ValidationLoop
            idPrefix="phase3"
            prompt={`${sharedContext}

PHASE 3: Strip renderer and camera

Own these files:
- src/gtk/workspace_strip.h
- src/gtk/workspace_strip.c

Build the first working strip layout backend core.

Required behavior:
- one notebook per column initially
- columns laid out horizontally in a strip
- active column kept visible via camera pan
- model-driven widths and target widths
- no Yoga dependency
- this phase is backend-focused; runtime activation and Docker/Wayland/prettymux-open verification belong to Phase 3B

Required functions:
- workspace_strip_init()
- workspace_strip_create_root()
- workspace_strip_apply_layout()
- workspace_strip_focus_column()
- workspace_strip_pan_to_focused_column()
- workspace_strip_clamp_camera()
- workspace_strip_tick_cb()

Keep the code reviewable:
- prefer simple geometry math over over-engineering
- add only the minimal animation state needed for camera and width interpolation

Run a build at the end. Return { summary, filesChanged, allTestsPassing }.`}
            implementAgents={agents.smartTool}
            reviewAgents={agents.reviewer}
            feedback={p3.feedback}
            done={p3.done}
            maxIterations={3}
          />
        </Sequence>

        <Sequence>
          <ValidationLoop
            idPrefix="phase3b"
            prompt={`${sharedContext}

PHASE 3B: Strip activation and runtime verification

Own these files:
- src/gtk/workspace.c
- src/gtk/socket_commands.c
- src/gtk/workspace_strip.c
- src/gtk/test_workspace_layout.c
- src/gtk/test_workspace_strip.c
- packaging/containers/debian-bookworm.Dockerfile
- packaging/containers/verify-strip-layout.sh

Make strip mode activatable in the real app flow and prove it works through containerized runtime verification.

Required outcomes:
- runtime entrypoints rebuild/activate the strip backend correctly
- focus lands on the real terminal, not a placeholder widget
- the container verification path launches a real PrettyMux instance
- the verification path prefers sharing the host Wayland session for local runs
- the verification path falls back to headless Weston only when host Wayland is unavailable
- prettymux-open talks to a live instance and exercises strip activation against that live instance

This phase is where Docker + Wayland + prettymux-open proof belongs. Do not satisfy it with CLI-only parsing checks.

Run a build and directly relevant tests at the end. Return { summary, filesChanged, allTestsPassing }.`}
            implementAgents={agents.smartTool}
            reviewAgents={agents.reviewer}
            feedback={p3b.feedback}
            done={p3b.done}
            maxIterations={3}
          />
        </Sequence>

        <Sequence>
          <ValidationLoop
            idPrefix="phase4"
            prompt={`${sharedContext}

PHASE 4: Action mapping and simple zoom semantics

Own these files:
- src/gtk/app_actions.c
- src/gtk/shortcuts.c
- src/gtk/workspace.c
- src/gtk/workspace_strip.c

Map existing user actions onto strip mode while preserving classic behavior.

Required behavior:
- classic mode keeps current Ctrl+Shift+Z behavior unchanged
- strip mode makes Ctrl+Shift+Z toggle maximize/unmaximize of the active column
- split-right in strip mode inserts a new column
- close current pane in strip mode removes or collapses the active column appropriately
- next/previous navigation works naturally across columns

Required functions:
- workspace_split_current_for_layout()
- workspace_close_current_for_layout()
- workspace_focus_next_for_layout()
- workspace_focus_prev_for_layout()
- workspace_strip_insert_column_after_active()
- workspace_strip_remove_active_column()
- workspace_strip_toggle_maximize_column()

Be explicit about unsupported strip-mode actions rather than faking broken behavior.

Run a build and any directly relevant tests at the end. Return { summary, filesChanged, allTestsPassing }.`}
            implementAgents={agents.smartTool}
            reviewAgents={agents.reviewer}
            feedback={p4.feedback}
            done={p4.done}
            maxIterations={3}
          />
        </Sequence>

        <Sequence>
          <ValidationLoop
            idPrefix="phase5"
            prompt={`${sharedContext}

PHASE 5: Session persistence for strip layouts

Own these files:
- src/gtk/session.c
- any required integration points in src/gtk/workspace.c or src/gtk/workspace_layout.c

Persist the new layout mode and strip state safely.

Required outcomes:
- classic sessions continue to use the existing schema safely
- strip sessions serialize layout_mode, columns, widths, focused column, and maximize state

Required functions to add or extend:
- session_save_workspace_layout_mode()
- session_save_workspace_strip_state()
- session_restore_workspace_strip_state()

Do not attempt to unify classic and strip into one fragile session schema.

Run a build and any directly relevant tests at the end. Return { summary, filesChanged, allTestsPassing }.`}
            implementAgents={agents.smartTool}
            reviewAgents={agents.reviewer}
            feedback={p5.feedback}
            done={p5.done}
            maxIterations={3}
          />
        </Sequence>

        <Sequence>
          <ValidationLoop
            idPrefix="phase5b"
            prompt={`${sharedContext}

PHASE 5B: Layout settings surface

Own these files:
- src/gtk/app_settings.h
- src/gtk/app_settings.c
- src/gtk/settings_dialog.c

Expose the layout-mode setting cleanly in the UI and settings layer.

Required outcomes:
- settings expose Classic vs Strip clearly
- default remains Classic
- wording does not imply strip is the only or preferred mode

Required functions to add or extend:
- app_settings_get_default_layout_mode()
- app_settings_set_default_layout_mode()

This phase owns the settings UI only. Do not drift back into session-schema work unless strictly required for a clean settings integration.

Run a build and any directly relevant tests at the end. Return { summary, filesChanged, allTestsPassing }.`}
            implementAgents={agents.smartTool}
            reviewAgents={agents.reviewer}
            feedback={p5b.feedback}
            done={p5b.done}
            maxIterations={3}
          />
        </Sequence>

        <Sequence>
          <ValidationLoop
            idPrefix="phase6"
            prompt={`${sharedContext}

PHASE 6: Multi-instance socket and command isolation

Own these files:
- src/gtk/main.c
- src/gtk/app_actions.c
- src/gtk/socket_commands.c
- src/gtk/socket_server.c
- src/gtk/app_state.c
- any launcher / CLI files that address the socket directly

Implement support for running more than one PrettyMux instance at the same time without cross-instance shortcut or socket interference.

Required outcomes:
- each running instance gets a stable instance identifier
- each instance owns its own socket path / command target
- remote commands can address a specific instance, with a sensible default when no target is given
- local in-process shortcuts stay local to the current instance

Required API / state:
- app_state_get_instance_id()
- socket_server_get_instance_socket_path()
- socket_server_route_command_to_instance()

Design constraints:
- avoid singleton assumptions that leak actions across processes
- keep socket routing for explicit remote control paths only
- do not regress the existing single-instance flow for users who only run one instance

This phase owns socket/instance routing only. Per-instance session persistence belongs to Phase 6B.

Run a build and any directly relevant tests at the end. Return { summary, filesChanged, allTestsPassing }.`}
            implementAgents={agents.smartTool}
            reviewAgents={agents.reviewer}
            feedback={p6.feedback}
            done={p6.done}
            maxIterations={3}
          />
        </Sequence>

        <Sequence>
          <ValidationLoop
            idPrefix="phase6b"
            prompt={`${sharedContext}

PHASE 6B: Per-instance session persistence

Own these files:
- src/gtk/session.c
- src/gtk/app_state.c
- src/gtk/main.c

Make session save/restore per instance rather than globally shared.

Required outcomes:
- each instance uses its own session path / last-session record
- restoring one instance does not overwrite another instance's state
- per-instance session naming survives restarts predictably

Required API / state:
- session_get_instance_session_path()
- session_save_for_instance()
- session_restore_for_instance()

This phase owns per-instance session persistence only. Do not drift back into general socket routing work except where a small integration point is required.

Run a build and any directly relevant tests at the end. Return { summary, filesChanged, allTestsPassing }.`}
            implementAgents={agents.smartTool}
            reviewAgents={agents.reviewer}
            feedback={p6b.feedback}
            done={p6b.done}
            maxIterations={3}
          />
        </Sequence>

        <Sequence>
          <ValidationLoop
            idPrefix="phase7"
            prompt={`${sharedContext}

PHASE 7: Agent metadata and status system

Own these files:
- src/gtk/workspace.c
- src/gtk/workspace.h
- src/gtk/sidebar_ui.c
- src/gtk/sidebar_ui.h
- src/gtk/notifications.c
- src/gtk/socket_commands.c
- any CLI / hook integration files that already surface agent state

Implement a richer per-workspace metadata model inspired by Séance.

Required outcomes:
- structured status entries instead of only flat branch/cwd summaries
- support multiple agent/status entries per workspace
- surface recent agent/session status in the sidebar without making rows unreadable
- integrate cleanly with existing PrettyMux notification behavior

Suggested API:
- workspace_status_entry
- workspace_set_status_entry()
- workspace_clear_status_entry()
- workspace_get_sorted_status_entries()
- sidebar_ui_build_workspace_status_section()

Constraints:
- keep the model generic for Claude, Codex, Pi, and future tools
- avoid hardcoding one provider's semantics into the core data model

Run a build and any directly relevant tests at the end. Return { summary, filesChanged, allTestsPassing }.`}
            implementAgents={agents.smartTool}
            reviewAgents={agents.reviewer}
            feedback={p7.feedback}
            done={p7.done}
            maxIterations={3}
          />
        </Sequence>

        <Sequence>
          <ValidationLoop
            idPrefix="phase8"
            prompt={`${sharedContext}

PHASE 8: Sidebar section expansion core

Own these files:
- src/gtk/sidebar_ui.c
- src/gtk/sidebar_ui.h
- src/gtk/theme.c
- src/gtk/workspace.c
- src/gtk/workspace.h

Expand the workspace cards into a fuller summary system inspired by Séance.

Sections to support:
- title row
- attention / unread indicator
- status entries
- recent notification preview
- branch + cwd

Requirements:
- rows must remain compact and scannable
- sections should be configurable or suppressible if they become noisy
- preserve rename, drag/reorder, selection, and close interactions

Suggested API:
- sidebar_ui_build_notification_preview_section()
- sidebar_ui_build_branch_cwd_section()

Keep this phase focused on the core rich sections above. Ports, structural indicators, and progress visualization belong to Phase 8B.

Run a build and any directly relevant tests at the end. Return { summary, filesChanged, allTestsPassing }.`}
            implementAgents={agents.smartTool}
            reviewAgents={agents.reviewer}
            feedback={p8.feedback}
            done={p8.done}
            maxIterations={3}
          />
        </Sequence>

        <Sequence>
          <ValidationLoop
            idPrefix="phase8b"
            prompt={`${sharedContext}

PHASE 8B: Sidebar auxiliary sections

Own these files:
- src/gtk/sidebar_ui.c
- src/gtk/sidebar_ui.h
- src/gtk/theme.c
- src/gtk/workspace.c
- src/gtk/workspace.h

Extend the richer sidebar cards with the more auxiliary sections inspired by Séance.

Sections to support:
- ports summary
- pane / column count indicator
- optional compact progress visualization if there is a clean PrettyMux signal for it

Requirements:
- keep rows compact and suppress noisy sections when data is absent
- preserve rename, drag/reorder, selection, and close interactions

Suggested API:
- sidebar_ui_build_ports_section()
- sidebar_ui_build_progress_section()
- sidebar_ui_build_structure_indicator_section()

Run a build and any directly relevant tests at the end. Return { summary, filesChanged, allTestsPassing }.`}
            implementAgents={agents.smartTool}
            reviewAgents={agents.reviewer}
            feedback={p8b.feedback}
            done={p8b.done}
            maxIterations={3}
          />
        </Sequence>

        <Sequence>
          <ValidationLoop
            idPrefix="phase9"
            prompt={`${sharedContext}

PHASE 9: Animation and detail pass

Own these files:
- src/gtk/workspace_strip.c
- src/gtk/sidebar_ui.c
- src/gtk/theme.c
- any focused animation helpers added during earlier phases

Refine strip mode and sidebar behavior with more polished animation/detail work inspired by Séance.

Targets:
- smoother camera easing
- cleaner column insertion/removal transitions
- more deliberate maximize/unmaximize transitions
- subtle sidebar row motion or reveal behavior where it adds clarity

Constraints:
- do not add animation that obscures state or harms responsiveness
- keep the code maintainable and avoid decorative overreach

Run a build and any directly relevant tests at the end. Return { summary, filesChanged, allTestsPassing }.`}
            implementAgents={agents.smartTool}
            reviewAgents={agents.reviewer}
            feedback={p9.feedback}
            done={p9.done}
            maxIterations={3}
          />
        </Sequence>

        <Sequence>
          <ValidationLoop
            idPrefix="phase10"
            prompt={`${sharedContext}

PHASE 10: Multi-window workspace movement backend

Own these files:
- src/gtk/main.c
- src/gtk/app_state.c
- src/gtk/app_actions.c
- src/gtk/workspace.c
- src/gtk/sidebar_ui.c
- src/gtk/socket_server.c
- any window-management modules involved in instance tracking

Implement controlled workspace movement between PrettyMux windows/instances at the backend/protocol level.

Required outcomes:
- user can move a workspace from one window/instance to another
- workspace identity and session data survive the move
- sidebar, notifications, and socket targeting update correctly after the move
- no action bleed between instances

Suggested API:
- app_state_list_instances()
- workspace_detach_from_instance()
- workspace_attach_to_instance()
- workspace_move_to_instance()

This phase owns the backend move/import/export path. Extending the existing move modal belongs to Phase 10B.

Run a build and any directly relevant tests at the end. Return { summary, filesChanged, allTestsPassing }.`}
            implementAgents={agents.smartTool}
            reviewAgents={agents.reviewer}
            feedback={p10.feedback}
            done={p10.done}
            maxIterations={3}
          />
        </Sequence>

        <Sequence>
          <ValidationLoop
            idPrefix="phase10b"
            prompt={`${sharedContext}

PHASE 10B: Move modal extension for other windows

Own these files:
- src/gtk/pane_move_overlay.c
- src/gtk/sidebar_ui.c
- any move-target modal/controller code involved today

Extend the existing move modal so it can target other windows/instances.

Required outcomes:
- do not create a separate move-to-window dialog
- show other windows/instances as additional move targets in the existing modal
- implement workspace-to-window first; tab/pane-to-window can follow later if serialization is not ready

Suggested API:
- sidebar_ui_show_move_to_window_menu()

This phase owns only the move-target UI/controller surface. Backend move/import/export logic belongs to Phase 10.

Run a build and any directly relevant tests at the end. Return { summary, filesChanged, allTestsPassing }.`}
            implementAgents={agents.smartTool}
            reviewAgents={agents.reviewer}
            feedback={p10b.feedback}
            done={p10b.done}
            maxIterations={3}
          />
        </Sequence>
          </>
        )}
      </Sequence>
    </Workflow>
  );
});
