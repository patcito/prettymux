// smithers-display-name: PrettyMux Review Recovery
/** @jsxImportSource smithers-orchestrator */
import { createSmithers } from "smithers-orchestrator";
import { z } from "zod/v4";
import { agents } from "~/agents";
import { ValidationLoop, implementOutputSchema, validateOutputSchema } from "~/components/ValidationLoop";
import { reviewOutputSchema } from "~/components/Review";

const inputSchema = z.object({
  prompt: z.string().default(
    "Fix the remaining PrettyMux review debt from .smithers/tickets/prettymux-review-recovery.md without reopening already accepted work.",
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

  const done = reviews.length > 0 && reviews.some((r: any) => r.approved === true);
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
    feedback:
      reviews
        .filter((r: any) => r.approved === false)
        .map((r: any) => `REVIEWER REJECTED:\n${r.feedback}`)
        .join("\n\n") || null,
  };
}

const sharedContext = `
SOURCE DOCUMENTS:
- Read .smithers/tickets/prettymux-review-recovery.md first.
- Use .smithers/tickets/prettymux-strip-layout-plan.md only as supporting background.

MISSION:
- Fix only the remaining unresolved review debt.
- Preserve already accepted runtime/layout/session behavior.

ALREADY ACCEPTED / DO NOT REOPEN WITHOUT CAUSE:
- recovery0
- recover-phase2
- recover-phase4
- recover-phase5
- recover-phase6b
- recover-phase8b

ONLY REMAINING TARGET:
- recover-phase1

CROSS-PHASE RULES:
- correctness, non-regression, and explicit proof are more important than perfect file ownership purity
- do not bundle broad unrelated refactors or opportunistic cleanup
- for user-visible work, Docker + Wayland live verification is mandatory
- use prettymux-open for scriptable behavior whenever sensible
- if a behavior is not reasonably scriptable, say so explicitly and cover it with focused tests instead
- do not regress the approved host-Wayland container verification path in packaging/containers/verify-strip-layout.sh

REQUIRED SUMMARY FORMAT:
- every implementation summary must include these headings exactly:
  - Changes
  - Validation
  - Live verification
- the "Live verification" section must include:
  - Command(s):
  - Environment:
  - Assertion(s):
  - Result:
`;

export default smithers((ctx) => {
  const p0 = preflightState(ctx, "recovery0");
  const p1 = phaseState(ctx, "recover-phase1");

  return (
    <Workflow name="prettymux-review-recovery">
      <Sequence>
        <Sequence>
          <ValidationLoop
            idPrefix="recovery0"
            prompt={`${sharedContext}

RECOVERY 0: Verification preflight (advisory)

Use this as an advisory runtime-environment check before the remaining review target.

Own these files only if a fix is required:
- packaging/containers/debian-bookworm.Dockerfile
- packaging/containers/verify-strip-layout.sh

Goal:
- prove the verification path still basically works before touching the remaining review debt

Required outcomes:
- local build works
- host-Wayland Docker verification still launches a live PrettyMux instance
- prettymux-open still talks to that live instance
- if the current verification path already works, prefer a no-op and report proof only

Hard rules:
- do not modify any file outside the two allowed ownership files
- do not touch src/gtk/* runtime code in this preflight
- do not treat unrelated existing tree dirtiness as a blocker
- this preflight is advisory, not blocking
- if the script exits non-zero after PrettyMux has launched and prettymux-open has already reached the live instance, report that explicitly and continue to Phase 1
- run and report this exact command:
  - bash packaging/containers/verify-strip-layout.sh

This preflight gets one iteration only.

Return exact commands used in the summary.`}
            implementAgents={agents.smartTool}
            reviewAgents={agents.reviewer}
            feedback={p0.feedback}
            done={p0.done}
            maxIterations={1}
          />
        </Sequence>

        <Sequence>
          <ValidationLoop
            idPrefix="recover-phase1"
            prompt={`${sharedContext}

RECOVER PHASE 1: Sidebar card UI

Read the "Phase 1: Sidebar Card UI" section in .smithers/tickets/prettymux-review-recovery.md.

Primary owned files:
- src/gtk/sidebar_ui.c
- src/gtk/sidebar_ui.h
- src/gtk/theme.c

Minimal adjacent integration changes are acceptable if they are directly required for correct integration or focused tests.

Goal:
- get the remaining Phase 1 sidebar-card UI review debt accepted

Required outcomes:
- card UI remains visibly upgraded
- preserved interactions are explicitly validated:
  - inline rename
  - selection
  - drag/reorder
  - close/delete
- do not reopen already accepted layout/session/socket behavior

Hard rules:
- do not do broad restructuring or opportunistic cleanup
- do not move large amounts of code into new modules in this phase
- if adjacent files are touched, the summary must explain why they were necessary for integration or proof
- because this is user-visible, include an explicit "Live verification" section
- if rename/drag are not scriptable, say that explicitly and state the focused GTK test that covers them
- reviewer note:
  - accept small adjacent wiring/test changes if the sidebar-card recovery is correct, non-regressive, and explicitly proven
  - reject for scope only if the change set materially obscures review or turns into unrelated restructuring

Return exact validation commands and what each one proves.`}
            implementAgents={agents.smartTool}
            reviewAgents={agents.reviewer}
            feedback={p1.feedback}
            done={p1.done}
            maxIterations={3}
          />
        </Sequence>
      </Sequence>
    </Workflow>
  );
});
