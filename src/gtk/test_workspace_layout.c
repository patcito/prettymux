/*
 * test_workspace_layout.c — tests for layout backend dispatch.
 *
 * Compiles workspace_layout.c with stubs for workspace.c and
 * workspace_strip.c functions.  Verifies mode get/set, null safety,
 * and correct dispatch to classic vs strip backends.
 */
#include "workspace_layout.h"
#include "workspace.h"
#include "workspace_strip.h"
#include <string.h>

/* ── Dispatch counters ─────────────────────────────────────────────── */

static int focus_first_terminal_called;
static int toggle_zoom_classic_called;
static int strip_create_root_called;
static int strip_focus_primary_called;
static int strip_toggle_zoom_called;

static void
reset_counters(void)
{
    focus_first_terminal_called = 0;
    toggle_zoom_classic_called = 0;
    strip_create_root_called = 0;
    strip_focus_primary_called = 0;
    strip_toggle_zoom_called = 0;
}

/* ── Stubs for functions called by workspace_layout.c ──────────────── */

void
workspace_focus_first_terminal(Workspace *ws)
{
    (void)ws;
    focus_first_terminal_called++;
}

void
workspace_toggle_zoom(Workspace *ws)
{
    (void)ws;
    toggle_zoom_classic_called++;
}

void
workspace_strip_state_free(WorkspaceStripState *state)
{
    (void)state;
}

GtkWidget *
workspace_strip_create_root(Workspace *ws, GtkWidget *first_notebook)
{
    (void)ws;
    strip_create_root_called++;
    return first_notebook;
}

void
workspace_strip_focus_primary(Workspace *ws)
{
    (void)ws;
    strip_focus_primary_called++;
}

void
workspace_strip_toggle_zoom(Workspace *ws)
{
    (void)ws;
    strip_toggle_zoom_called++;
}

void
workspace_strip_toggle_half_maximize_column(Workspace *ws)
{
    (void)ws;
}

void
workspace_strip_add_notebook_column(Workspace *ws, GtkWidget *notebook)
{
    (void)ws;
    (void)notebook;
}

int
workspace_get_pane_index(Workspace *ws, GtkNotebook *pane)
{
    (void)ws;
    (void)pane;
    return -1;
}

/* ── Helpers ───────────────────────────────────────────────────────── */

static Workspace *
make_workspace(WorkspaceLayoutMode mode)
{
    Workspace *ws = g_new0(Workspace, 1);
    ws->layout_mode = mode;
    return ws;
}

/* ── Tests: get/set ────────────────────────────────────────────────── */

static void
test_get_layout_mode_default(void)
{
    Workspace *ws = make_workspace(WORKSPACE_LAYOUT_CLASSIC);
    g_assert_cmpint(workspace_get_layout_mode(ws), ==, WORKSPACE_LAYOUT_CLASSIC);
    g_free(ws);
}

static void
test_get_layout_mode_null(void)
{
    g_assert_cmpint(workspace_get_layout_mode(NULL), ==, WORKSPACE_LAYOUT_CLASSIC);
}

static void
test_set_layout_mode(void)
{
    Workspace *ws = make_workspace(WORKSPACE_LAYOUT_CLASSIC);

    workspace_set_layout_mode(ws, WORKSPACE_LAYOUT_STRIP);
    g_assert_cmpint(workspace_get_layout_mode(ws), ==, WORKSPACE_LAYOUT_STRIP);

    workspace_set_layout_mode(ws, WORKSPACE_LAYOUT_CLASSIC);
    g_assert_cmpint(workspace_get_layout_mode(ws), ==, WORKSPACE_LAYOUT_CLASSIC);

    g_free(ws);
}

static void
test_set_layout_mode_null(void)
{
    workspace_set_layout_mode(NULL, WORKSPACE_LAYOUT_STRIP);
}

/* ── Tests: create_root dispatch ───────────────────────────────────── */

static void
test_create_root_null(void)
{
    reset_counters();
    GtkWidget *dummy = (GtkWidget *)(gintptr)0x1;
    g_assert_true(workspace_layout_create_root(NULL, dummy) == dummy);
    g_assert_cmpint(strip_create_root_called, ==, 0);
}

static void
test_create_root_classic(void)
{
    Workspace *ws = make_workspace(WORKSPACE_LAYOUT_CLASSIC);
    GtkWidget *dummy = (GtkWidget *)(gintptr)0x1;

    reset_counters();
    GtkWidget *result = workspace_layout_create_root(ws, dummy);

    g_assert_true(result == dummy);
    g_assert_cmpint(strip_create_root_called, ==, 0);

    g_free(ws);
}

static void
test_create_root_strip(void)
{
    Workspace *ws = make_workspace(WORKSPACE_LAYOUT_STRIP);
    GtkWidget *dummy = (GtkWidget *)(gintptr)0x1;

    reset_counters();
    GtkWidget *result = workspace_layout_create_root(ws, dummy);

    g_assert_true(result == dummy);
    g_assert_cmpint(strip_create_root_called, ==, 1);

    g_free(ws);
}

/* ── Tests: focus_primary dispatch ─────────────────────────────────── */

static void
test_focus_primary_null(void)
{
    reset_counters();
    workspace_layout_focus_primary(NULL);
    g_assert_cmpint(focus_first_terminal_called, ==, 0);
    g_assert_cmpint(strip_focus_primary_called, ==, 0);
}

static void
test_focus_primary_classic(void)
{
    Workspace *ws = make_workspace(WORKSPACE_LAYOUT_CLASSIC);
    reset_counters();
    workspace_layout_focus_primary(ws);
    g_assert_cmpint(focus_first_terminal_called, ==, 1);
    g_assert_cmpint(strip_focus_primary_called, ==, 0);
    g_free(ws);
}

static void
test_focus_primary_strip(void)
{
    Workspace *ws = make_workspace(WORKSPACE_LAYOUT_STRIP);
    reset_counters();
    workspace_layout_focus_primary(ws);
    g_assert_cmpint(focus_first_terminal_called, ==, 0);
    g_assert_cmpint(strip_focus_primary_called, ==, 1);
    g_free(ws);
}

/* ── Tests: toggle_zoom dispatch ───────────────────────────────────── */

static void
test_toggle_zoom_null(void)
{
    reset_counters();
    workspace_layout_toggle_zoom_current(NULL);
    g_assert_cmpint(toggle_zoom_classic_called, ==, 0);
    g_assert_cmpint(strip_toggle_zoom_called, ==, 0);
}

static void
test_toggle_zoom_classic(void)
{
    Workspace *ws = make_workspace(WORKSPACE_LAYOUT_CLASSIC);
    reset_counters();
    workspace_layout_toggle_zoom_current(ws);
    g_assert_cmpint(toggle_zoom_classic_called, ==, 1);
    g_assert_cmpint(strip_toggle_zoom_called, ==, 0);
    g_free(ws);
}

static void
test_toggle_zoom_strip(void)
{
    Workspace *ws = make_workspace(WORKSPACE_LAYOUT_STRIP);
    reset_counters();
    workspace_layout_toggle_zoom_current(ws);
    g_assert_cmpint(toggle_zoom_classic_called, ==, 0);
    g_assert_cmpint(strip_toggle_zoom_called, ==, 1);
    g_free(ws);
}

/* ── Tests: is_zoomed dispatch ─────────────────────────────────────── */

static void
test_is_zoomed_null(void)
{
    g_assert_false(workspace_layout_is_zoomed(NULL));
}

static void
test_is_zoomed_classic_not_zoomed(void)
{
    Workspace *ws = make_workspace(WORKSPACE_LAYOUT_CLASSIC);
    ws->zoomed = FALSE;
    g_assert_false(workspace_layout_is_zoomed(ws));
    g_free(ws);
}

static void
test_is_zoomed_classic_zoomed(void)
{
    Workspace *ws = make_workspace(WORKSPACE_LAYOUT_CLASSIC);
    ws->zoomed = TRUE;
    g_assert_true(workspace_layout_is_zoomed(ws));
    g_free(ws);
}

static void
test_is_zoomed_strip_no_state(void)
{
    Workspace *ws = make_workspace(WORKSPACE_LAYOUT_STRIP);
    g_assert_false(workspace_layout_is_zoomed(ws));
    g_free(ws);
}

static void
test_is_zoomed_strip_not_maximized(void)
{
    Workspace *ws = make_workspace(WORKSPACE_LAYOUT_STRIP);
    WorkspaceStripState state = {0};
    WorkspaceColumn col = { .notebook = NULL, .target_width = 400,
                            .current_width = 400.0, .maximized = FALSE };
    WorkspaceColumn *colp = &col;
    state.columns = g_ptr_array_new();
    g_ptr_array_add(state.columns, colp);
    state.focused_col = 0;
    ws->strip_state = &state;
    g_assert_false(workspace_layout_is_zoomed(ws));
    g_ptr_array_free(state.columns, TRUE);
    ws->strip_state = NULL;
    g_free(ws);
}

static void
test_is_zoomed_strip_maximized(void)
{
    Workspace *ws = make_workspace(WORKSPACE_LAYOUT_STRIP);
    WorkspaceStripState state = {0};
    WorkspaceColumn col = { .notebook = NULL, .target_width = 800,
                            .current_width = 800.0, .maximized = TRUE };
    WorkspaceColumn *colp = &col;
    state.columns = g_ptr_array_new();
    g_ptr_array_add(state.columns, colp);
    state.focused_col = 0;
    ws->strip_state = &state;
    g_assert_true(workspace_layout_is_zoomed(ws));
    g_ptr_array_free(state.columns, TRUE);
    ws->strip_state = NULL;
    g_free(ws);
}

/* ── Tests: rebuild guardrails ────────────────────────────────────── */

static void
test_rebuild_null(void)
{
    g_assert_false(workspace_rebuild_for_layout_mode(NULL,
                                                     WORKSPACE_LAYOUT_STRIP));
}

static void
test_rebuild_same_mode(void)
{
    Workspace *ws = make_workspace(WORKSPACE_LAYOUT_CLASSIC);
    g_assert_true(workspace_rebuild_for_layout_mode(ws,
                                                    WORKSPACE_LAYOUT_CLASSIC));
    g_free(ws);
}

static void
test_rebuild_missing_overlay(void)
{
    Workspace *ws = make_workspace(WORKSPACE_LAYOUT_CLASSIC);
    ws->notebook = (GtkWidget *)(gintptr)0x1;
    g_assert_false(workspace_rebuild_for_layout_mode(ws,
                                                     WORKSPACE_LAYOUT_STRIP));
    g_free(ws);
}

static void
test_rebuild_missing_notebook(void)
{
    Workspace *ws = make_workspace(WORKSPACE_LAYOUT_CLASSIC);
    ws->overlay = (GtkWidget *)(gintptr)0x1;
    g_assert_false(workspace_rebuild_for_layout_mode(ws,
                                                     WORKSPACE_LAYOUT_STRIP));
    g_free(ws);
}

/* ── main ──────────────────────────────────────────────────────────── */

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/workspace-layout/get-mode/default",
                    test_get_layout_mode_default);
    g_test_add_func("/workspace-layout/get-mode/null",
                    test_get_layout_mode_null);
    g_test_add_func("/workspace-layout/set-mode/round-trip",
                    test_set_layout_mode);
    g_test_add_func("/workspace-layout/set-mode/null",
                    test_set_layout_mode_null);

    g_test_add_func("/workspace-layout/create-root/null",
                    test_create_root_null);
    g_test_add_func("/workspace-layout/create-root/classic",
                    test_create_root_classic);
    g_test_add_func("/workspace-layout/create-root/strip",
                    test_create_root_strip);

    g_test_add_func("/workspace-layout/focus-primary/null",
                    test_focus_primary_null);
    g_test_add_func("/workspace-layout/focus-primary/classic",
                    test_focus_primary_classic);
    g_test_add_func("/workspace-layout/focus-primary/strip",
                    test_focus_primary_strip);

    g_test_add_func("/workspace-layout/toggle-zoom/null",
                    test_toggle_zoom_null);
    g_test_add_func("/workspace-layout/toggle-zoom/classic",
                    test_toggle_zoom_classic);
    g_test_add_func("/workspace-layout/toggle-zoom/strip",
                    test_toggle_zoom_strip);
    g_test_add_func("/workspace-layout/is-zoomed/null",
                    test_is_zoomed_null);
    g_test_add_func("/workspace-layout/is-zoomed/classic-not-zoomed",
                    test_is_zoomed_classic_not_zoomed);
    g_test_add_func("/workspace-layout/is-zoomed/classic-zoomed",
                    test_is_zoomed_classic_zoomed);
    g_test_add_func("/workspace-layout/is-zoomed/strip-no-state",
                    test_is_zoomed_strip_no_state);
    g_test_add_func("/workspace-layout/is-zoomed/strip-not-maximized",
                    test_is_zoomed_strip_not_maximized);
    g_test_add_func("/workspace-layout/is-zoomed/strip-maximized",
                    test_is_zoomed_strip_maximized);
    g_test_add_func("/workspace-layout/rebuild/null",
                    test_rebuild_null);
    g_test_add_func("/workspace-layout/rebuild/same-mode",
                    test_rebuild_same_mode);
    g_test_add_func("/workspace-layout/rebuild/missing-overlay",
                    test_rebuild_missing_overlay);
    g_test_add_func("/workspace-layout/rebuild/missing-notebook",
                    test_rebuild_missing_notebook);

    return g_test_run();
}
