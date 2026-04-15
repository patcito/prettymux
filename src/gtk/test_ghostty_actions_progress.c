#include "ghostty_actions.h"

#include <gtk/gtk.h>

#include "app_actions.h"
#include "app_state.h"
#include "app_support.h"
#include "app_ui.h"
#include "ghostty_terminal.h"
#include "notifications.h"
#include "terminal_routing.h"
#include "workspace.h"

int current_workspace = 0;

static AppState g_state = {0};
static ghostty_surface_t g_surface = (ghostty_surface_t)(uintptr_t)0x1;
static GhosttyTerminal *g_terminal = (GhosttyTerminal *)(uintptr_t)0x2;
static Workspace g_workspace = {0};
static int g_set_progress_calls = 0;
static int g_last_progress_state = -1;
static int g_last_progress_percent = -2;
static int g_sidebar_refresh_calls = 0;
static int g_tab_refresh_calls = 0;

static void
drain_main_context(void)
{
    while (g_main_context_pending(NULL))
        g_main_context_iteration(NULL, FALSE);
}

static void
reset_test_state(void)
{
    memset(&g_workspace, 0, sizeof(g_workspace));
    g_set_progress_calls = 0;
    g_last_progress_state = -1;
    g_last_progress_percent = -2;
    g_sidebar_refresh_calls = 0;
    g_tab_refresh_calls = 0;
}

AppState *
app_state(void)
{
    return &g_state;
}

void
app_actions_open_url_in_preferred_target(const char *url)
{
    (void)url;
}

void
debug_notification_log(const char *fmt, ...)
{
    (void)fmt;
}

void
send_desktop_notification(const char *title,
                          const char *body,
                          int ws_idx,
                          int pane_idx,
                          int tab_idx)
{
    (void)title;
    (void)body;
    (void)ws_idx;
    (void)pane_idx;
    (void)tab_idx;
}

void
bell_button_update(void)
{
}

void
notifications_add_full(const char *msg,
                       int ws_idx,
                       GtkNotebook *pane,
                       int tab_idx)
{
    (void)msg;
    (void)ws_idx;
    (void)pane;
    (void)tab_idx;
}

gboolean
notification_target_is_active(int ws_idx, GtkNotebook *pane_notebook, int tab_idx)
{
    (void)ws_idx;
    (void)pane_notebook;
    (void)tab_idx;
    return FALSE;
}

void
sidebar_toast_show(const char *msg,
                   int ws_idx,
                   GtkNotebook *pane_notebook,
                   int tab_idx)
{
    (void)msg;
    (void)ws_idx;
    (void)pane_notebook;
    (void)tab_idx;
}

SurfaceLookup
terminal_routing_find_for_surface(ghostty_surface_t surface)
{
    SurfaceLookup loc = {0};

    if (surface != g_surface)
        return loc;

    loc.terminal = g_terminal;
    loc.workspace = &g_workspace;
    loc.workspace_idx = 0;
    loc.pane_notebook = NULL;
    loc.pane_idx = 0;
    loc.tab_idx = 0;
    return loc;
}

void
ghostty_terminal_set_hover_url(GhosttyTerminal *self, const char *url)
{
    (void)self;
    (void)url;
}

void
ghostty_terminal_set_title(GhosttyTerminal *self, const char *title)
{
    (void)self;
    (void)title;
}

void
ghostty_terminal_set_cwd(GhosttyTerminal *self, const char *cwd)
{
    (void)self;
    (void)cwd;
}

const char *
ghostty_terminal_get_title(GhosttyTerminal *self)
{
    (void)self;
    return "Terminal";
}

void
ghostty_terminal_set_status(GhosttyTerminal *self,
                            const char *cwd,
                            const char *git_branch)
{
    (void)self;
    (void)cwd;
    (void)git_branch;
}

void
ghostty_terminal_notify_command_finished(GhosttyTerminal *self,
                                         int exit_code,
                                         uint64_t duration_ns)
{
    (void)self;
    (void)exit_code;
    (void)duration_ns;
}

void
ghostty_terminal_notify_bell(GhosttyTerminal *self)
{
    (void)self;
}

void
ghostty_terminal_queue_render(GhosttyTerminal *self)
{
    (void)self;
}

void
ghostty_terminal_mark_activity(GhosttyTerminal *self)
{
    (void)self;
}

void
ghostty_terminal_notify_child_exited(GhosttyTerminal *self, uint32_t exit_code)
{
    (void)self;
    (void)exit_code;
}

void
ghostty_terminal_set_progress(GhosttyTerminal *self, int state, int percent)
{
    (void)self;
    g_set_progress_calls++;
    g_last_progress_state = state;
    g_last_progress_percent = percent;
}

void
ghostty_terminal_set_search_results(GhosttyTerminal *self,
                                    gint64 total,
                                    gint64 selected)
{
    (void)self;
    (void)total;
    (void)selected;
}

void
terminal_search_show(GhosttyTerminal *term, const char *needle)
{
    (void)term;
    (void)needle;
}

void
workspace_detect_git(Workspace *ws)
{
    (void)ws;
}

void
workspace_mark_tab_notification(GtkNotebook *pane, int page_num)
{
    (void)pane;
    (void)page_num;
}

GtkNotebook *
workspace_get_focused_pane(Workspace *ws)
{
    (void)ws;
    return NULL;
}

GhosttyTerminal *
notebook_terminal_at(GtkNotebook *notebook, int page_num)
{
    (void)notebook;
    (void)page_num;
    return NULL;
}

void
workspace_refresh_tab_labels(Workspace *ws)
{
    g_assert_true(ws == &g_workspace);
    g_tab_refresh_calls++;
}

void
workspace_refresh_sidebar_label(Workspace *ws)
{
    g_assert_true(ws == &g_workspace);
    g_sidebar_refresh_calls++;
}

static void
test_progress_report_updates_sidebar_refresh(void)
{
    ghostty_target_s target = {0};
    ghostty_action_s action = {0};

    reset_test_state();
    g_strlcpy(g_workspace.notification, "stale", sizeof(g_workspace.notification));

    target.tag = GHOSTTY_TARGET_SURFACE;
    target.target.surface = g_surface;
    action.tag = GHOSTTY_ACTION_PROGRESS_REPORT;
    action.action.progress_report.state = GHOSTTY_PROGRESS_STATE_SET;
    action.action.progress_report.progress = 62;

    g_assert_true(ghostty_actions_action_cb(NULL, target, action));
    drain_main_context();

    g_assert_cmpint(g_set_progress_calls, ==, 1);
    g_assert_cmpint(g_last_progress_state, ==, GHOSTTY_PROGRESS_STATE_SET);
    g_assert_cmpint(g_last_progress_percent, ==, 62);
    g_assert_cmpint(g_tab_refresh_calls, ==, 1);
    g_assert_cmpint(g_sidebar_refresh_calls, ==, 1);
    g_assert_cmpstr(g_workspace.notification, ==, "Progress: 62%");
}

static void
test_progress_report_clears_notification_for_unknown_percent(void)
{
    ghostty_target_s target = {0};
    ghostty_action_s action = {0};

    reset_test_state();
    g_strlcpy(g_workspace.notification, "will-clear",
              sizeof(g_workspace.notification));

    target.tag = GHOSTTY_TARGET_SURFACE;
    target.target.surface = g_surface;
    action.tag = GHOSTTY_ACTION_PROGRESS_REPORT;
    action.action.progress_report.state = GHOSTTY_PROGRESS_STATE_PAUSE;
    action.action.progress_report.progress = -1;

    g_assert_true(ghostty_actions_action_cb(NULL, target, action));
    drain_main_context();

    g_assert_cmpint(g_set_progress_calls, ==, 1);
    g_assert_cmpint(g_last_progress_state, ==, GHOSTTY_PROGRESS_STATE_PAUSE);
    g_assert_cmpint(g_last_progress_percent, ==, -1);
    g_assert_cmpint(g_tab_refresh_calls, ==, 1);
    g_assert_cmpint(g_sidebar_refresh_calls, ==, 1);
    g_assert_cmpstr(g_workspace.notification, ==, "");
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/ghostty-actions/progress/sidebar-refresh",
                    test_progress_report_updates_sidebar_refresh);
    g_test_add_func("/ghostty-actions/progress/unknown-clears-notification",
                    test_progress_report_clears_notification_for_unknown_percent);

    return g_test_run();
}
