#include "workspace.h"
#include "workspace_layout.h"
#include "workspace_strip.h"
#include "app_settings.h"
#include "close_confirm.h"
#include "ghostty_terminal.h"
#include "hover_focus.h"
#include "port_scanner.h"
#include "project_icon_cache.h"
#include "resize_overlay.h"
#include "session.h"
#include "shortcuts.h"
#include "sidebar_data.h"
#include "sidebar_sections.h"
#include "sidebar_ui.h"

#include <string.h>

typedef struct {
    Workspace *workspace;
} SidebarCtxDataMirror;

static int session_queue_save_call_count = 0;
static int terminal_request_close_call_count = 0;
static int terminal_hangup_session_call_count = 0;

/* ---- External stubs required by workspace.c ---- */

ghostty_app_t g_ghostty_app = NULL;
float g_ghostty_default_font_size = 12.0f;

gboolean
app_settings_get_focus_on_hover(void)
{
    return FALSE;
}

WorkspaceLayoutMode
app_settings_get_default_layout_mode(void)
{
    return WORKSPACE_LAYOUT_CLASSIC;
}

void
close_confirm_request(GtkWindow *parent,
                      CloseConfirmKind kind,
                      CloseConfirmCallback callback,
                      gpointer user_data,
                      GDestroyNotify destroy)
{
    (void)parent;
    (void)kind;
    if (callback)
        callback(TRUE, user_data);
    if (destroy)
        destroy(user_data);
}

gboolean
hover_focus_should_enter(void)
{
    return FALSE;
}

void
hover_focus_note_pointer_motion(void)
{
}

void
port_scanner_set_active_workspace(int workspace_idx)
{
    (void)workspace_idx;
}

char *
project_icon_cache_root_for_path(const char *path)
{
    (void)path;
    return NULL;
}

const char *
project_icon_cache_lookup(const char *root)
{
    (void)root;
    return NULL;
}

const char *
project_icon_cache_lookup_for_path(const char *path)
{
    (void)path;
    return NULL;
}

void
project_icon_cache_request(const char *path,
                           ProjectIconResolvedFunc callback,
                           gpointer user_data,
                           GDestroyNotify destroy)
{
    (void)path;
    if (callback)
        callback(NULL, NULL, user_data);
    if (destroy)
        destroy(user_data);
}

void
resize_overlay_connect_paned(GtkPaned *paned)
{
    (void)paned;
}

void
session_queue_save(void)
{
    session_queue_save_call_count++;
}

void
notifications_on_workspace_removed(int removed_ws_idx)
{
    (void)removed_ws_idx;
}

void
shortcut_log_event(const char *type, const char *action, const char *keys)
{
    (void)type;
    (void)action;
    (void)keys;
}

char *
sidebar_data_format_status(int pane_count, int tab_count)
{
    (void)pane_count;
    (void)tab_count;
    return g_strdup("");
}

const char *
sidebar_data_resolve_branch(const char *primary_branch,
                            const char *fallback_branch,
                            gboolean has_first_terminal)
{
    (void)has_first_terminal;
    if (primary_branch && primary_branch[0])
        return primary_branch;
    if (fallback_branch && fallback_branch[0])
        return fallback_branch;
    return NULL;
}

GtkWidget *
sidebar_ui_build_workspace_card(GtkWidget *header_box,
                                GtkWidget **out_meta_label,
                                GtkWidget **out_status_label,
                                GtkWidget **out_status_entries_box,
                                GtkWidget **out_ports_label,
                                GtkWidget **out_progress_label,
                                GtkWidget **out_structure_label,
                                GtkWidget **out_badge)
{
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *details = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *meta = gtk_label_new(NULL);
    GtkWidget *status = gtk_label_new(NULL);
    GtkWidget *status_entries = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *aux = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *ports = gtk_label_new(NULL);
    GtkWidget *progress = gtk_label_new(NULL);
    GtkWidget *structure = gtk_label_new(NULL);
    GtkWidget *badge = gtk_label_new("!");

    g_object_set_data(G_OBJECT(card), "header-box", header_box);
    gtk_box_append(GTK_BOX(card), header_box);
    gtk_box_append(GTK_BOX(card), details);
    gtk_box_append(GTK_BOX(details), meta);
    gtk_box_append(GTK_BOX(details), status_entries);
    gtk_box_append(GTK_BOX(details), status);
    gtk_box_append(GTK_BOX(details), aux);
    gtk_box_append(GTK_BOX(aux), structure);
    gtk_box_append(GTK_BOX(aux), ports);
    gtk_box_append(GTK_BOX(aux), progress);
    gtk_box_append(GTK_BOX(header_box), badge);

    if (out_meta_label)
        *out_meta_label = meta;
    if (out_status_label)
        *out_status_label = status;
    if (out_status_entries_box)
        *out_status_entries_box = status_entries;
    if (out_ports_label)
        *out_ports_label = ports;
    if (out_progress_label)
        *out_progress_label = progress;
    if (out_structure_label)
        *out_structure_label = structure;
    if (out_badge)
        *out_badge = badge;

    return card;
}

void
sidebar_ui_build_workspace_status_section(GtkWidget *section_box,
                                          GPtrArray *status_entries,
                                          int max_entries)
{
    (void)section_box;
    (void)status_entries;
    (void)max_entries;
}

void
sidebar_ui_build_notification_preview_section(GtkWidget *section_label,
                                              const char *preview,
                                              gboolean enabled)
{
    (void)section_label;
    (void)preview;
    (void)enabled;
}

void
sidebar_ui_build_branch_cwd_section(GtkWidget *section_label,
                                    const char *cwd,
                                    const char *branch,
                                    gboolean enabled)
{
    (void)section_label;
    (void)cwd;
    (void)branch;
    (void)enabled;
}

void
sidebar_ui_build_ports_section(GtkWidget *section_label,
                               const char *ports_summary,
                               gboolean enabled)
{
    (void)section_label;
    (void)ports_summary;
    (void)enabled;
}

void
sidebar_ui_build_progress_section(GtkWidget *section_label,
                                  int progress_state,
                                  int progress_percent,
                                  gboolean enabled)
{
    (void)section_label;
    (void)progress_state;
    (void)progress_percent;
    (void)enabled;
}

void
sidebar_ui_build_structure_indicator_section(GtkWidget *section_label,
                                             gboolean strip_mode,
                                             int pane_or_column_count,
                                             int tab_count,
                                             gboolean enabled)
{
    (void)section_label;
    (void)strip_mode;
    (void)pane_or_column_count;
    (void)tab_count;
    (void)enabled;
}

void
sidebar_ui_show_move_to_window_menu(Workspace *workspace)
{
    (void)workspace;
}

void
sidebar_ui_cancel_pending_activate(void)
{
}

static gsize terminal_signals_registered = 0;

static void
ensure_terminal_signals_registered(void)
{
    if (g_once_init_enter(&terminal_signals_registered)) {
        g_signal_new("title-changed",
                     GTK_TYPE_LABEL,
                     G_SIGNAL_RUN_LAST,
                     0,
                     NULL, NULL, NULL,
                     G_TYPE_NONE,
                     0);
        g_signal_new("pwd-changed",
                     GTK_TYPE_LABEL,
                     G_SIGNAL_RUN_LAST,
                     0,
                     NULL, NULL, NULL,
                     G_TYPE_NONE,
                     0);
        g_once_init_leave(&terminal_signals_registered, 1);
    }
}

GType
ghostty_terminal_get_type(void)
{
    ensure_terminal_signals_registered();
    return GTK_TYPE_LABEL;
}

GtkWidget *
ghostty_terminal_new(const char *start_cwd)
{
    GtkWidget *terminal;

    ensure_terminal_signals_registered();
    terminal = gtk_label_new("stub-terminal");
    gtk_label_set_text(GTK_LABEL(terminal), "stub-terminal");
    g_object_set_data_full(G_OBJECT(terminal), "stub-cwd",
                           g_strdup((start_cwd && start_cwd[0]) ? start_cwd : "/tmp"),
                           g_free);
    g_object_set_data_full(G_OBJECT(terminal), "stub-title",
                           g_strdup("stub-title"),
                           g_free);
    g_object_set_data(G_OBJECT(terminal), "stub-session-id",
                      GINT_TO_POINTER(0));
    g_object_set_data(G_OBJECT(terminal), "stub-process-exited",
                      GINT_TO_POINTER(0));
    return terminal;
}

const char *
ghostty_terminal_get_cwd(GhosttyTerminal *self)
{
    const char *cwd = g_object_get_data(G_OBJECT(self), "stub-cwd");
    return (cwd && cwd[0]) ? cwd : "/tmp";
}

const char *
ghostty_terminal_get_title(GhosttyTerminal *self)
{
    const char *title = g_object_get_data(G_OBJECT(self), "stub-title");
    return (title && title[0]) ? title : "stub-title";
}

pid_t
ghostty_terminal_get_session_id(GhosttyTerminal *self)
{
    return (pid_t)GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(self), "stub-session-id"));
}

void
ghostty_terminal_set_dummy_target(GhosttyTerminal *self, GtkWidget *dummy)
{
    g_object_set_data(G_OBJECT(self), "stub-dummy-target", dummy);
}

GtkWidget *
ghostty_terminal_get_dummy_target(GhosttyTerminal *self)
{
    return g_object_get_data(G_OBJECT(self), "stub-dummy-target");
}

void
ghostty_terminal_focus(GhosttyTerminal *self)
{
    (void)self;
}

void
ghostty_terminal_queue_render(GhosttyTerminal *self)
{
    (void)self;
}

void
ghostty_terminal_request_close(GhosttyTerminal *self)
{
    terminal_request_close_call_count++;
    g_object_set_data(G_OBJECT(self), "stub-close-requested",
                      GINT_TO_POINTER(1));
}

gboolean
ghostty_terminal_process_exited(GhosttyTerminal *self)
{
    return GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(self), "stub-process-exited")) != 0;
}

gboolean
ghostty_terminal_hangup_session(GhosttyTerminal *self)
{
    pid_t sid = ghostty_terminal_get_session_id(self);

    if (sid <= 0)
        return FALSE;

    terminal_hangup_session_call_count++;
    g_object_set_data(G_OBJECT(self), "stub-session-hung-up",
                      GINT_TO_POINTER(1));
    return TRUE;
}

void
ghostty_terminal_clear_activity(GhosttyTerminal *self)
{
    (void)self;
}

gboolean
ghostty_terminal_has_activity(GhosttyTerminal *self)
{
    (void)self;
    return FALSE;
}

int
ghostty_terminal_get_progress_percent(GhosttyTerminal *self)
{
    (void)self;
    return -1;
}

int
ghostty_terminal_get_progress_state(GhosttyTerminal *self)
{
    (void)self;
    return -1;
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

/* ---- Test helpers ---- */

static gboolean have_display;

static void
require_display_or_skip(void)
{
    if (!have_display)
        g_test_skip("requires GTK display");
}

static void
drain_main_context(void)
{
    while (g_main_context_iteration(NULL, FALSE))
        ;
}

static GtkNotebook *
make_notebook_with_terminal(const char *tab_label)
{
    GtkNotebook *nb = GTK_NOTEBOOK(gtk_notebook_new());
    GtkWidget *terminal = ghostty_terminal_new(NULL);
    GtkWidget *tab = gtk_label_new(tab_label ? tab_label : "tab");

    gtk_notebook_append_page(nb, terminal, tab);
    gtk_notebook_set_current_page(nb, 0);
    return nb;
}

static Workspace *
alloc_workspace(void)
{
    Workspace *ws = g_new0(Workspace, 1);
    ws->pane_notebooks = g_ptr_array_new();
    ws->terminals = g_ptr_array_new();
    return ws;
}

static void
free_workspace_fixture(Workspace *ws)
{
    if (!ws)
        return;
    if (ws->git_branch_cancel) {
        g_cancellable_cancel(ws->git_branch_cancel);
        g_clear_object(&ws->git_branch_cancel);
    }
    if (ws->primary_branch_cancel) {
        g_cancellable_cancel(ws->primary_branch_cancel);
        g_clear_object(&ws->primary_branch_cancel);
    }
    if (ws->overlay)
        g_object_unref(ws->overlay);
    if (ws->strip_state)
        workspace_strip_state_free(ws->strip_state);
    if (ws->pane_notebooks)
        g_ptr_array_unref(ws->pane_notebooks);
    if (ws->terminals)
        g_ptr_array_unref(ws->terminals);
    if (ws->status_entries)
        g_hash_table_unref(ws->status_entries);
    g_free(ws);
}

static void
free_workspace_fixture_without_async_cancel(Workspace *ws)
{
    if (!ws)
        return;
    g_clear_object(&ws->git_branch_cancel);
    g_clear_object(&ws->primary_branch_cancel);
    if (ws->overlay)
        g_object_unref(ws->overlay);
    if (ws->strip_state)
        workspace_strip_state_free(ws->strip_state);
    if (ws->pane_notebooks)
        g_ptr_array_unref(ws->pane_notebooks);
    if (ws->terminals)
        g_ptr_array_unref(ws->terminals);
    if (ws->status_entries)
        g_hash_table_unref(ws->status_entries);
    g_free(ws);
}

static void
reset_workspace_globals(void)
{
    if (workspaces) {
        for (guint i = 0; i < workspaces->len; i++) {
            Workspace *ws = g_ptr_array_index(workspaces, i);
            free_workspace_fixture(ws);
        }
        g_ptr_array_unref(workspaces);
        workspaces = NULL;
    }
    current_workspace = 0;
    g_terminal_stack = NULL;
    g_workspace_list = NULL;
    session_queue_save_call_count = 0;
    terminal_request_close_call_count = 0;
    terminal_hangup_session_call_count = 0;
}

static GtkEventController *
find_controller_by_type(GtkWidget *widget, GType controller_type)
{
    g_autoptr(GListModel) controllers = NULL;

    if (!GTK_IS_WIDGET(widget))
        return NULL;

    controllers = gtk_widget_observe_controllers(widget);
    for (guint i = 0; i < g_list_model_get_n_items(controllers); i++) {
        GtkEventController *controller = g_list_model_get_item(controllers, i);
        if (G_TYPE_CHECK_INSTANCE_TYPE(controller, controller_type))
            return controller;
        g_object_unref(controller);
    }

    return NULL;
}

static Workspace *
make_strip_workspace(int column_count)
{
    Workspace *ws = alloc_workspace();
    GtkNotebook *first_nb = make_notebook_with_terminal("c0");

    ws->layout_mode = WORKSPACE_LAYOUT_CLASSIC;
    ws->overlay = NULL;
    ws->notebook = GTK_WIDGET(first_nb);
    ws->active_pane = first_nb;

    g_ptr_array_add(ws->pane_notebooks, first_nb);
    g_ptr_array_add(ws->terminals, gtk_notebook_get_nth_page(first_nb, 0));

    workspace_strip_create_root(ws, GTK_WIDGET(first_nb));

    for (int i = 1; i < column_count; i++) {
        char tab_name[16];
        GtkNotebook *nb;
        snprintf(tab_name, sizeof(tab_name), "c%d", i);
        nb = make_notebook_with_terminal(tab_name);
        g_ptr_array_add(ws->pane_notebooks, nb);
        g_ptr_array_add(ws->terminals, gtk_notebook_get_nth_page(nb, 0));
        workspace_strip_add_notebook_column(ws, GTK_WIDGET(nb));
    }

    workspace_strip_focus_column(ws, 0);
    return ws;
}

static Workspace *
make_classic_workspace_with_tabs(int tab_count)
{
    Workspace *ws = alloc_workspace();
    GtkNotebook *nb = GTK_NOTEBOOK(gtk_notebook_new());

    ws->layout_mode = WORKSPACE_LAYOUT_CLASSIC;
    ws->overlay = NULL;
    ws->notebook = GTK_WIDGET(nb);
    ws->active_pane = nb;

    for (int i = 0; i < tab_count; i++) {
        char tab_name[16];
        GtkWidget *terminal;
        GtkWidget *tab;

        snprintf(tab_name, sizeof(tab_name), "t%d", i);
        terminal = ghostty_terminal_new(NULL);
        tab = gtk_label_new(tab_name);
        gtk_notebook_append_page(nb, terminal, tab);
        g_ptr_array_add(ws->terminals, terminal);
    }

    g_ptr_array_add(ws->pane_notebooks, nb);
    gtk_notebook_set_current_page(nb, 0);
    return ws;
}

static Workspace *
make_classic_workspace_with_two_panes(void)
{
    Workspace *ws = alloc_workspace();
    GtkNotebook *nb0 = make_notebook_with_terminal("left");
    GtkNotebook *nb1 = make_notebook_with_terminal("right");
    GtkWidget *overlay = gtk_overlay_new();
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);

    ws->layout_mode = WORKSPACE_LAYOUT_CLASSIC;
    ws->overlay = NULL;
    ws->notebook = GTK_WIDGET(nb0);
    ws->active_pane = nb0;

    g_ptr_array_add(ws->pane_notebooks, nb0);
    g_ptr_array_add(ws->pane_notebooks, nb1);
    g_ptr_array_add(ws->terminals, gtk_notebook_get_nth_page(nb0, 0));
    g_ptr_array_add(ws->terminals, gtk_notebook_get_nth_page(nb1, 0));

    gtk_paned_set_start_child(GTK_PANED(paned), GTK_WIDGET(nb0));
    gtk_paned_set_end_child(GTK_PANED(paned), GTK_WIDGET(nb1));
    gtk_overlay_set_child(GTK_OVERLAY(overlay), paned);
    return ws;
}

static Workspace *
make_workspace_with_linked_tabs(const char *name, int tab_count)
{
    Workspace *ws = alloc_workspace();
    GtkNotebook *nb = GTK_NOTEBOOK(gtk_notebook_new());

    if (name && name[0])
        g_strlcpy(ws->name, name, sizeof(ws->name));
    ws->layout_mode = WORKSPACE_LAYOUT_CLASSIC;
    ws->notebook = GTK_WIDGET(nb);
    ws->active_pane = nb;
    g_object_set_data(G_OBJECT(nb), "workspace-ptr", ws);

    for (int i = 0; i < tab_count; i++) {
        char tab_name[16];
        GtkWidget *terminal = ghostty_terminal_new(NULL);
        GtkWidget *dummy = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        GtkWidget *tab;

        snprintf(tab_name, sizeof(tab_name), "t%d", i);
        tab = gtk_label_new(tab_name);
        g_object_set_data(G_OBJECT(dummy), "linked-terminal", terminal);
        g_object_set_data(G_OBJECT(terminal), "linked-dummy", dummy);
        ghostty_terminal_set_dummy_target(GHOSTTY_TERMINAL(terminal), dummy);
        gtk_notebook_append_page(nb, dummy, tab);
        g_ptr_array_add(ws->terminals, terminal);
    }

    g_ptr_array_add(ws->pane_notebooks, nb);
    gtk_notebook_set_current_page(nb, 0);
    return ws;
}

static void
init_two_workspace_runtime(GtkWidget **terminal_stack_out,
                           GtkWidget **workspace_list_out)
{
    GtkWidget *terminal_stack = gtk_stack_new();
    GtkWidget *workspace_list = gtk_list_box_new();

    workspace_add(terminal_stack, workspace_list, NULL);
    workspace_add(terminal_stack, workspace_list, NULL);

    if (terminal_stack_out)
        *terminal_stack_out = terminal_stack;
    if (workspace_list_out)
        *workspace_list_out = workspace_list;
}

static GtkWidget *
tab_inner_label(GtkWidget *tab_widget)
{
    for (GtkWidget *child = gtk_widget_get_first_child(tab_widget);
         child;
         child = gtk_widget_get_next_sibling(child)) {
        if (GTK_IS_LABEL(child))
            return child;
    }
    return NULL;
}

static GtkWidget *
tab_emoji_label(GtkWidget *tab_widget)
{
    GtkWidget *icon_stack = g_object_get_data(G_OBJECT(tab_widget),
                                              "row-icon-widget");
    if (!GTK_IS_STACK(icon_stack))
        return NULL;
    return g_object_get_data(G_OBJECT(icon_stack), "row-icon-label");
}

static const ShortcutDef *
default_shortcut_for_action(const char *action)
{
    for (int i = 0; default_shortcuts[i].action != NULL; i++) {
        if (g_strcmp0(default_shortcuts[i].action, action) == 0)
            return &default_shortcuts[i];
    }
    return NULL;
}

/* ---- Phase 4 API tests ---- */

static void
test_split_current_for_layout_strip_horizontal_inserts_column(void)
{
    require_display_or_skip();

    Workspace *ws = make_strip_workspace(1);

    g_assert_cmpuint(ws->pane_notebooks->len, ==, 1);
    g_assert_cmpuint(ws->strip_state->columns->len, ==, 1);

    g_assert_true(workspace_split_current_for_layout(ws,
                                                     GTK_ORIENTATION_HORIZONTAL,
                                                     NULL));
    g_assert_cmpuint(ws->pane_notebooks->len, ==, 2);
    g_assert_cmpuint(ws->strip_state->columns->len, ==, 2);
    g_assert_cmpint(ws->strip_state->focused_col, ==, 1);

    free_workspace_fixture(ws);
}

static void
test_split_current_for_layout_strip_vertical_stacks_in_column(void)
{
    require_display_or_skip();

    Workspace *ws = make_strip_workspace(1);

    g_assert_cmpuint(ws->pane_notebooks->len, ==, 1);
    g_assert_cmpuint(ws->strip_state->columns->len, ==, 1);

    g_assert_true(workspace_split_current_for_layout(ws,
                                                     GTK_ORIENTATION_VERTICAL,
                                                     NULL));
    g_assert_cmpuint(ws->pane_notebooks->len, ==, 2);
    g_assert_cmpuint(ws->strip_state->columns->len, ==, 1);

    WorkspaceColumn *col = g_ptr_array_index(ws->strip_state->columns, 0);
    g_assert_nonnull(col->panes);
    g_assert_cmpuint(col->panes->len, ==, 2);
    g_assert_cmpint(col->focused_pane, ==, 1);

    free_workspace_fixture(ws);
}

static void
test_close_current_for_layout_strip_removes_active_column(void)
{
    require_display_or_skip();

    Workspace *ws = make_strip_workspace(2);
    GtkNotebook *first = g_ptr_array_index(ws->pane_notebooks, 0);

    workspace_strip_focus_column(ws, 1);
    g_assert_true(workspace_close_current_for_layout(ws));
    g_assert_cmpuint(ws->pane_notebooks->len, ==, 1);
    g_assert_cmpuint(ws->strip_state->columns->len, ==, 1);
    g_assert_cmpint(ws->strip_state->focused_col, ==, 0);
    g_assert_true(ws->active_pane == first);

    free_workspace_fixture(ws);
}

static void
test_close_current_for_layout_strip_stacked_pane_keeps_column(void)
{
    require_display_or_skip();

    Workspace *ws = make_strip_workspace(2);
    WorkspaceColumn *col0;
    WorkspaceColumn *col1;
    GtkNotebook *remaining_col0;
    GtkNotebook *other_col;

    workspace_strip_focus_column(ws, 0);
    g_assert_true(workspace_split_current_for_layout(ws,
                                                     GTK_ORIENTATION_VERTICAL,
                                                     NULL));
    g_assert_cmpuint(ws->pane_notebooks->len, ==, 3);
    g_assert_cmpuint(ws->strip_state->columns->len, ==, 2);
    g_assert_cmpint(workspace_strip_column_pane_count(ws, 0), ==, 2);
    g_assert_cmpint(workspace_strip_column_pane_count(ws, 1), ==, 1);

    col1 = g_ptr_array_index(ws->strip_state->columns, 1);
    other_col = workspace_strip_column_focused_notebook(col1);

    g_assert_true(workspace_close_current_for_layout(ws));
    g_assert_cmpuint(ws->pane_notebooks->len, ==, 2);
    g_assert_cmpuint(ws->strip_state->columns->len, ==, 2);
    g_assert_cmpint(ws->strip_state->focused_col, ==, 0);
    g_assert_cmpint(workspace_strip_column_pane_count(ws, 0), ==, 1);
    g_assert_cmpint(workspace_strip_column_pane_count(ws, 1), ==, 1);

    col0 = g_ptr_array_index(ws->strip_state->columns, 0);
    remaining_col0 = workspace_strip_column_focused_notebook(col0);
    g_assert_nonnull(remaining_col0);
    g_assert_true(ws->active_pane == remaining_col0);
    g_assert_true(ws->active_pane != other_col);

    free_workspace_fixture(ws);
}

static void
test_close_current_for_layout_strip_single_column_fails(void)
{
    require_display_or_skip();

    Workspace *ws = make_strip_workspace(1);

    g_assert_false(workspace_close_current_for_layout(ws));
    g_assert_cmpuint(ws->pane_notebooks->len, ==, 1);
    g_assert_cmpuint(ws->strip_state->columns->len, ==, 1);

    free_workspace_fixture(ws);
}

static void
test_strip_shortcuts_match_phase4_contract(void)
{
    const ShortcutDef *split_horizontal;
    const ShortcutDef *focus_left;
    const ShortcutDef *focus_right;
    const ShortcutDef *focus_up;
    const ShortcutDef *focus_down;

    split_horizontal = default_shortcut_for_action("split.horizontal");
    focus_left = default_shortcut_for_action("pane.focus.left");
    focus_right = default_shortcut_for_action("pane.focus.right");
    focus_up = default_shortcut_for_action("pane.focus.up");
    focus_down = default_shortcut_for_action("pane.focus.down");

    g_assert_nonnull(split_horizontal);
    g_assert_cmpint(split_horizontal->keyval, ==, GDK_KEY_Return);
    g_assert_cmpuint(split_horizontal->mods,
                     ==, GDK_CONTROL_MASK | GDK_SHIFT_MASK);

    g_assert_nonnull(focus_left);
    g_assert_cmpint(focus_left->keyval, ==, GDK_KEY_Left);
    g_assert_cmpuint(focus_left->mods, ==, GDK_CONTROL_MASK | GDK_SHIFT_MASK);

    g_assert_nonnull(focus_right);
    g_assert_cmpint(focus_right->keyval, ==, GDK_KEY_Right);
    g_assert_cmpuint(focus_right->mods, ==, GDK_CONTROL_MASK | GDK_SHIFT_MASK);

    g_assert_nonnull(focus_up);
    g_assert_cmpint(focus_up->keyval, ==, GDK_KEY_Up);
    g_assert_cmpuint(focus_up->mods, ==, GDK_CONTROL_MASK | GDK_SHIFT_MASK);

    g_assert_nonnull(focus_down);
    g_assert_cmpint(focus_down->keyval, ==, GDK_KEY_Down);
    g_assert_cmpuint(focus_down->mods, ==, GDK_CONTROL_MASK | GDK_SHIFT_MASK);
}

static void
test_close_current_for_layout_classic_collapses_tree(void)
{
    require_display_or_skip();

    Workspace *ws = make_classic_workspace_with_two_panes();

    g_assert_cmpuint(ws->pane_notebooks->len, ==, 2);
    g_assert_true(workspace_close_current_for_layout(ws));
    g_assert_cmpuint(ws->pane_notebooks->len, ==, 1);

    free_workspace_fixture(ws);
}

static void
test_focus_next_prev_for_layout_strip_navigates_columns(void)
{
    require_display_or_skip();

    Workspace *ws = make_strip_workspace(3);

    g_assert_cmpint(ws->strip_state->focused_col, ==, 0);
    g_assert_true(workspace_focus_next_for_layout(ws));
    g_assert_cmpint(ws->strip_state->focused_col, ==, 1);
    g_assert_true(workspace_focus_prev_for_layout(ws));
    g_assert_cmpint(ws->strip_state->focused_col, ==, 0);

    free_workspace_fixture(ws);
}

static void
test_focus_next_prev_for_layout_strip_single_column_noop_success(void)
{
    require_display_or_skip();

    Workspace *ws = make_strip_workspace(1);
    GtkNotebook *only = ws->active_pane;

    g_assert_cmpint(ws->strip_state->focused_col, ==, 0);
    g_assert_true(workspace_focus_next_for_layout(ws));
    g_assert_cmpint(ws->strip_state->focused_col, ==, 0);
    g_assert_true(ws->active_pane == only);

    g_assert_true(workspace_focus_prev_for_layout(ws));
    g_assert_cmpint(ws->strip_state->focused_col, ==, 0);
    g_assert_true(ws->active_pane == only);

    free_workspace_fixture(ws);
}

static void
test_focus_next_prev_for_layout_classic_navigates_tabs(void)
{
    require_display_or_skip();

    Workspace *ws = make_classic_workspace_with_tabs(3);
    GtkNotebook *nb = ws->active_pane;

    gtk_notebook_set_current_page(nb, 0);
    g_assert_true(workspace_focus_next_for_layout(ws));
    g_assert_cmpint(gtk_notebook_get_current_page(nb), ==, 1);
    g_assert_true(workspace_focus_prev_for_layout(ws));
    g_assert_cmpint(gtk_notebook_get_current_page(nb), ==, 0);

    free_workspace_fixture(ws);
}

static void
test_workspace_status_entries_sorted_attention_then_recency(void)
{
    Workspace *ws = alloc_workspace();
    workspace_status_entry running = {0};
    workspace_status_entry blocked = {0};
    workspace_status_entry done = {0};
    g_autoptr(GPtrArray) entries = NULL;
    workspace_status_entry *e0;
    workspace_status_entry *e1;
    workspace_status_entry *e2;
    gint64 now = g_get_real_time();

    g_strlcpy(running.entry_id, "codex:run", sizeof(running.entry_id));
    g_strlcpy(running.provider, "codex", sizeof(running.provider));
    g_strlcpy(running.status, "running", sizeof(running.status));
    g_strlcpy(running.summary, "indexing", sizeof(running.summary));
    running.updated_at_usec = now - 40 * G_USEC_PER_SEC;

    g_strlcpy(blocked.entry_id, "claude:block", sizeof(blocked.entry_id));
    g_strlcpy(blocked.provider, "claude", sizeof(blocked.provider));
    g_strlcpy(blocked.status, "blocked", sizeof(blocked.status));
    g_strlcpy(blocked.summary, "waiting for review",
              sizeof(blocked.summary));
    blocked.updated_at_usec = now - 10 * G_USEC_PER_SEC;
    blocked.attention = TRUE;

    g_strlcpy(done.entry_id, "pi:done", sizeof(done.entry_id));
    g_strlcpy(done.provider, "pi", sizeof(done.provider));
    g_strlcpy(done.status, "done", sizeof(done.status));
    g_strlcpy(done.summary, "tests passed", sizeof(done.summary));
    done.updated_at_usec = now - 20 * G_USEC_PER_SEC;

    workspace_set_status_entry(ws, &running);
    workspace_set_status_entry(ws, &blocked);
    workspace_set_status_entry(ws, &done);

    entries = workspace_get_sorted_status_entries(ws);
    g_assert_nonnull(entries);
    g_assert_cmpuint(entries->len, ==, 3);

    e0 = g_ptr_array_index(entries, 0);
    e1 = g_ptr_array_index(entries, 1);
    e2 = g_ptr_array_index(entries, 2);
    g_assert_cmpstr(e0->entry_id, ==, "claude:block");
    g_assert_cmpstr(e1->entry_id, ==, "pi:done");
    g_assert_cmpstr(e2->entry_id, ==, "codex:run");

    free_workspace_fixture(ws);
}

static void
test_workspace_status_entry_clear_removes_entry(void)
{
    Workspace *ws = alloc_workspace();
    workspace_status_entry entry = {0};
    g_autoptr(GPtrArray) entries = NULL;

    g_strlcpy(entry.entry_id, "codex:main", sizeof(entry.entry_id));
    g_strlcpy(entry.provider, "codex", sizeof(entry.provider));
    g_strlcpy(entry.summary, "running", sizeof(entry.summary));

    workspace_set_status_entry(ws, &entry);
    entries = workspace_get_sorted_status_entries(ws);
    g_assert_nonnull(entries);
    g_assert_cmpuint(entries->len, ==, 1);

    workspace_clear_status_entry(ws, "codex:main");
    g_clear_pointer(&entries, g_ptr_array_unref);
    entries = workspace_get_sorted_status_entries(ws);
    g_assert_nonnull(entries);
    g_assert_cmpuint(entries->len, ==, 0);

    free_workspace_fixture(ws);
}

static void
test_workspace_move_tab_moves_linked_terminal_between_workspaces(void)
{
    Workspace *src;
    Workspace *dest;
    GtkNotebook *src_nb;
    GtkNotebook *dest_nb;

    require_display_or_skip();

    src = make_workspace_with_linked_tabs("src", 1);
    dest = make_workspace_with_linked_tabs("dest", 1);

    workspaces = g_ptr_array_new();
    g_ptr_array_add(workspaces, src);
    g_ptr_array_add(workspaces, dest);
    current_workspace = 0;

    src_nb = g_ptr_array_index(src->pane_notebooks, 0);
    dest_nb = g_ptr_array_index(dest->pane_notebooks, 0);
    g_assert_cmpint(gtk_notebook_get_n_pages(src_nb), ==, 1);
    g_assert_cmpint(gtk_notebook_get_n_pages(dest_nb), ==, 1);

    g_assert_true(workspace_move_tab(0, 0, 0, 1, 0));
    g_assert_cmpint(gtk_notebook_get_n_pages(src_nb), ==, 0);
    g_assert_cmpint(gtk_notebook_get_n_pages(dest_nb), ==, 2);

    free_workspace_fixture(src);
    free_workspace_fixture(dest);
    g_ptr_array_unref(workspaces);
    workspaces = NULL;
}

static void
test_workspace_select_tab_switches_target_workspace(void)
{
    Workspace *ws0;
    Workspace *ws1;
    GtkWidget *terminal_stack;
    GtkWidget *workspace_list;
    GtkWidget *row0;
    GtkWidget *row1;
    GtkListBoxRow *selected;

    require_display_or_skip();

    ws0 = make_workspace_with_linked_tabs("ws0", 1);
    ws1 = make_workspace_with_linked_tabs("ws1", 1);
    terminal_stack = gtk_stack_new();
    workspace_list = gtk_list_box_new();

    ws0->container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    ws1->container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_stack_add_named(GTK_STACK(terminal_stack), ws0->container, "ws-0");
    gtk_stack_add_named(GTK_STACK(terminal_stack), ws1->container, "ws-1");

    row0 = gtk_list_box_row_new();
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row0),
                               gtk_label_new("ws0"));
    gtk_list_box_append(GTK_LIST_BOX(workspace_list), row0);
    row1 = gtk_list_box_row_new();
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row1),
                               gtk_label_new("ws1"));
    gtk_list_box_append(GTK_LIST_BOX(workspace_list), row1);

    workspaces = g_ptr_array_new();
    g_ptr_array_add(workspaces, ws0);
    g_ptr_array_add(workspaces, ws1);
    current_workspace = 0;
    g_terminal_stack = terminal_stack;
    g_workspace_list = workspace_list;

    g_assert_true(workspace_select_tab(1, 0, 0));
    g_assert_cmpint(current_workspace, ==, 1);
    selected = gtk_list_box_get_selected_row(GTK_LIST_BOX(workspace_list));
    g_assert_nonnull(selected);
    g_assert_cmpint(gtk_list_box_row_get_index(selected), ==, 1);

    g_terminal_stack = NULL;
    g_workspace_list = NULL;
    free_workspace_fixture(ws0);
    free_workspace_fixture(ws1);
    g_ptr_array_unref(workspaces);
    workspaces = NULL;
}

static void
test_workspace_close_current_tab_closes_when_multiple_tabs(void)
{
    Workspace *ws;
    GtkNotebook *nb;

    require_display_or_skip();

    ws = make_workspace_with_linked_tabs("close", 2);
    nb = g_ptr_array_index(ws->pane_notebooks, 0);
    gtk_notebook_set_current_page(nb, 1);

    g_assert_true(workspace_close_current_tab(ws));
    g_assert_cmpint(gtk_notebook_get_n_pages(nb), ==, 1);
    g_assert_false(workspace_close_current_tab(ws));
    drain_main_context();

    free_workspace_fixture(ws);
}

static void
test_workspace_remove_deletes_sidebar_workspace_row(void)
{
    Workspace *ws0;
    Workspace *ws1;
    GtkWidget *terminal_stack;
    GtkWidget *workspace_list;
    GtkWidget *row0;
    GtkWidget *row1;

    require_display_or_skip();

    ws0 = make_workspace_with_linked_tabs("ws0", 1);
    ws1 = make_workspace_with_linked_tabs("ws1", 1);
    terminal_stack = gtk_stack_new();
    workspace_list = gtk_list_box_new();

    ws0->container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    ws1->container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_stack_add_named(GTK_STACK(terminal_stack), ws0->container, "ws-0");
    gtk_stack_add_named(GTK_STACK(terminal_stack), ws1->container, "ws-1");

    row0 = gtk_list_box_row_new();
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row0), gtk_label_new("ws0"));
    gtk_list_box_append(GTK_LIST_BOX(workspace_list), row0);
    row1 = gtk_list_box_row_new();
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row1), gtk_label_new("ws1"));
    gtk_list_box_append(GTK_LIST_BOX(workspace_list), row1);

    workspaces = g_ptr_array_new();
    g_ptr_array_add(workspaces, ws0);
    g_ptr_array_add(workspaces, ws1);
    current_workspace = 0;

    workspace_remove(1, terminal_stack, workspace_list);
    g_assert_cmpuint(workspaces->len, ==, 1);
    g_assert_nonnull(gtk_list_box_get_row_at_index(GTK_LIST_BOX(workspace_list), 0));
    g_assert_null(gtk_list_box_get_row_at_index(GTK_LIST_BOX(workspace_list), 1));

    free_workspace_fixture(g_ptr_array_index(workspaces, 0));
    g_ptr_array_unref(workspaces);
    workspaces = NULL;
}

static void
test_workspace_sidebar_card_drop_target_moves_tab_between_workspaces(void)
{
    GtkWidget *terminal_stack;
    GtkWidget *workspace_list;
    Workspace *src_ws;
    Workspace *dest_ws;
    GtkNotebook *src_nb;
    GtkNotebook *dest_nb;
    GtkListBoxRow *dest_row;
    GtkWidget *dest_card;
    GtkEventController *drop_controller;

    require_display_or_skip();
    reset_workspace_globals();

    init_two_workspace_runtime(&terminal_stack, &workspace_list);
    src_ws = g_ptr_array_index(workspaces, 0);
    dest_ws = g_ptr_array_index(workspaces, 1);
    src_nb = g_ptr_array_index(src_ws->pane_notebooks, 0);
    dest_nb = g_ptr_array_index(dest_ws->pane_notebooks, 0);
    dest_row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(workspace_list), 1);
    g_assert_nonnull(dest_row);
    dest_card = gtk_list_box_row_get_child(dest_row);
    g_assert_nonnull(dest_card);
    drop_controller = find_controller_by_type(dest_card, GTK_TYPE_DROP_TARGET);
    g_assert_nonnull(drop_controller);

    g_assert_cmpint(gtk_notebook_get_n_pages(src_nb), ==, 1);
    g_assert_cmpint(gtk_notebook_get_n_pages(dest_nb), ==, 1);

    g_object_unref(drop_controller);

    g_assert_true(workspace_move_tab(0, 0, 0, 1, 0));
    g_assert_cmpint(gtk_notebook_get_n_pages(src_nb), ==, 0);
    g_assert_cmpint(gtk_notebook_get_n_pages(dest_nb), ==, 2);

    reset_workspace_globals();
    (void)terminal_stack;
}

static void
test_workspace_sidebar_card_select_tab_switches_workspace(void)
{
    GtkWidget *terminal_stack;
    GtkWidget *workspace_list;
    GtkListBoxRow *selected;

    require_display_or_skip();
    reset_workspace_globals();

    init_two_workspace_runtime(&terminal_stack, &workspace_list);
    g_assert_true(workspace_select_tab(1, 0, 0));
    g_assert_cmpint(current_workspace, ==, 1);
    selected = gtk_list_box_get_selected_row(GTK_LIST_BOX(workspace_list));
    g_assert_nonnull(selected);
    g_assert_cmpint(gtk_list_box_row_get_index(selected), ==, 1);
    Workspace *target_ws = g_ptr_array_index(workspaces, 1);
    g_assert_true(gtk_stack_get_visible_child(GTK_STACK(terminal_stack)) ==
                  target_ws->container);

    reset_workspace_globals();
}

static void
test_workspace_sidebar_card_delete_action_removes_workspace(void)
{
    GtkWidget *terminal_stack;
    GtkWidget *workspace_list;
    GtkListBoxRow *delete_row;
    GtkWidget *delete_card;
    SidebarCtxDataMirror *ctx;

    require_display_or_skip();
    reset_workspace_globals();

    init_two_workspace_runtime(&terminal_stack, &workspace_list);
    delete_row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(workspace_list), 1);
    g_assert_nonnull(delete_row);
    delete_card = gtk_list_box_row_get_child(delete_row);
    g_assert_nonnull(delete_card);
    ctx = g_object_get_data(G_OBJECT(delete_card), "sidebar-ctx-data");
    g_assert_nonnull(ctx);
    g_assert_true(ctx->workspace == g_ptr_array_index(workspaces, 1));
    g_assert_cmpint(workspace_get_index(ctx->workspace), ==, 1);

    workspace_remove(workspace_get_index(ctx->workspace),
                     terminal_stack, workspace_list);

    g_assert_cmpuint(workspaces->len, ==, 1);
    g_assert_nonnull(gtk_list_box_get_row_at_index(GTK_LIST_BOX(workspace_list), 0));
    g_assert_null(gtk_list_box_get_row_at_index(GTK_LIST_BOX(workspace_list), 1));

    reset_workspace_globals();
    (void)terminal_stack;
}

static void
test_workspace_sidebar_card_reorder_keeps_drop_controller_available(void)
{
    GtkWidget *terminal_stack;
    GtkWidget *workspace_list;
    Workspace *ws;
    GtkNotebook *nb;
    GtkWidget *first_page;
    GtkListBoxRow *row;
    GtkWidget *card;
    GtkEventController *drop_controller;

    require_display_or_skip();
    reset_workspace_globals();

    terminal_stack = gtk_stack_new();
    workspace_list = gtk_list_box_new();
    workspace_add(terminal_stack, workspace_list, NULL);
    ws = g_ptr_array_index(workspaces, 0);
    workspace_add_terminal(ws, NULL);
    nb = g_ptr_array_index(ws->pane_notebooks, 0);
    g_assert_cmpint(gtk_notebook_get_n_pages(nb), ==, 2);

    first_page = gtk_notebook_get_nth_page(nb, 0);
    gtk_notebook_reorder_child(nb, first_page, 1);
    g_assert_true(gtk_notebook_get_nth_page(nb, 1) == first_page);

    row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(workspace_list), 0);
    g_assert_nonnull(row);
    card = gtk_list_box_row_get_child(row);
    g_assert_nonnull(card);
    drop_controller = find_controller_by_type(card, GTK_TYPE_DROP_TARGET);
    g_assert_nonnull(drop_controller);
    g_object_unref(drop_controller);

    reset_workspace_globals();
}

static void
test_workspace_sidebar_card_double_click_starts_inline_rename(void)
{
    GtkWidget *terminal_stack;
    GtkWidget *workspace_list;
    Workspace *target_ws;
    GtkListBoxRow *row;
    GtkWidget *card;
    GtkWidget *header_box;
    GtkGestureClick *click;
    GtkWidget *rename_entry;

    require_display_or_skip();
    reset_workspace_globals();

    init_two_workspace_runtime(&terminal_stack, &workspace_list);
    workspace_switch(0, terminal_stack, workspace_list);
    g_assert_cmpint(current_workspace, ==, 0);

    target_ws = g_ptr_array_index(workspaces, 1);
    row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(workspace_list), 1);
    g_assert_nonnull(row);
    card = gtk_list_box_row_get_child(row);
    g_assert_nonnull(card);
    header_box = g_object_get_data(G_OBJECT(card), "header-box");
    g_assert_nonnull(header_box);

    click = GTK_GESTURE_CLICK(g_object_get_data(G_OBJECT(header_box),
                                                "rename-click-controller"));
    g_assert_true(GTK_IS_GESTURE_CLICK(click));
    g_signal_emit_by_name(click, "pressed", 2, 0.0, 0.0);

    rename_entry = g_object_get_data(G_OBJECT(header_box), "rename-entry");
    g_assert_true(GTK_IS_ENTRY(rename_entry));
    g_assert_cmpint(current_workspace, ==, 0);
    g_assert_cmpstr(target_ws->name, ==, "Workspace 2");
    g_assert_cmpint(session_queue_save_call_count, ==, 0);

    reset_workspace_globals();
}

static void
test_workspace_terminal_tab_double_click_starts_inline_rename(void)
{
    GtkWidget *terminal_stack;
    GtkWidget *workspace_list;
    Workspace *ws;
    GtkNotebook *nb;
    GtkWidget *page;
    GtkWidget *tab_widget;
    GtkGestureClick *click;
    GtkWidget *rename_entry;

    require_display_or_skip();
    reset_workspace_globals();

    init_two_workspace_runtime(&terminal_stack, &workspace_list);
    workspace_switch(0, terminal_stack, workspace_list);

    ws = g_ptr_array_index(workspaces, 0);
    nb = g_ptr_array_index(ws->pane_notebooks, 0);
    g_assert_true(GTK_IS_NOTEBOOK(nb));
    page = gtk_notebook_get_nth_page(nb, 0);
    g_assert_nonnull(page);
    tab_widget = gtk_notebook_get_tab_label(nb, page);
    g_assert_nonnull(tab_widget);

    click = GTK_GESTURE_CLICK(g_object_get_data(G_OBJECT(tab_widget),
                                                "rename-click-controller"));
    g_assert_true(GTK_IS_GESTURE_CLICK(click));
    g_signal_emit_by_name(click, "pressed", 2, 0.0, 0.0);

    rename_entry = g_object_get_data(G_OBJECT(tab_widget), "rename-entry");
    g_assert_true(GTK_IS_ENTRY(rename_entry));
    g_assert_cmpint(current_workspace, ==, 0);

    reset_workspace_globals();
}

static void
test_workspace_sidebar_card_rename_focus_leave_commits_and_saves(void)
{
    GtkWidget *terminal_stack;
    GtkWidget *workspace_list;
    Workspace *target_ws;
    GtkListBoxRow *row;
    GtkWidget *card;
    GtkWidget *header_box;
    GtkWidget *rename_entry;
    GtkWidget *rename_label;
    GtkGestureClick *click;
    GtkEventController *focus_ctrl;

    require_display_or_skip();
    reset_workspace_globals();

    init_two_workspace_runtime(&terminal_stack, &workspace_list);
    workspace_switch(0, terminal_stack, workspace_list);

    target_ws = g_ptr_array_index(workspaces, 1);
    row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(workspace_list), 1);
    g_assert_nonnull(row);
    card = gtk_list_box_row_get_child(row);
    g_assert_nonnull(card);
    header_box = g_object_get_data(G_OBJECT(card), "header-box");
    g_assert_nonnull(header_box);
    rename_label = target_ws->sidebar_label;
    g_assert_true(GTK_IS_LABEL(rename_label));

    click = GTK_GESTURE_CLICK(g_object_get_data(G_OBJECT(header_box),
                                                "rename-click-controller"));
    g_assert_true(GTK_IS_GESTURE_CLICK(click));
    g_signal_emit_by_name(click, "pressed", 2, 0.0, 0.0);

    rename_entry = g_object_get_data(G_OBJECT(header_box), "rename-entry");
    g_assert_true(GTK_IS_ENTRY(rename_entry));
    gtk_editable_set_text(GTK_EDITABLE(rename_entry), "renamed sidebar");

    focus_ctrl = g_object_get_data(G_OBJECT(rename_entry),
                                   "rename-focus-controller");
    g_assert_true(GTK_IS_EVENT_CONTROLLER_FOCUS(focus_ctrl));
    g_signal_emit_by_name(focus_ctrl, "leave");

    g_assert_null(g_object_get_data(G_OBJECT(header_box), "rename-entry"));
    g_assert_cmpstr(target_ws->name, ==, "renamed sidebar");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(rename_label)), ==,
                    "renamed sidebar");
    g_assert_cmpint(session_queue_save_call_count, ==, 1);
    g_assert_cmpint(current_workspace, ==, 0);

    reset_workspace_globals();
}

static void
test_workspace_sidebar_card_rename_escape_cancels_without_save(void)
{
    GtkWidget *terminal_stack;
    GtkWidget *workspace_list;
    Workspace *target_ws;
    GtkListBoxRow *row;
    GtkWidget *card;
    GtkWidget *header_box;
    GtkWidget *rename_entry;
    GtkWidget *rename_label;
    GtkGestureClick *click;
    GtkEventController *key_ctrl;
    gboolean handled = FALSE;

    require_display_or_skip();
    reset_workspace_globals();

    init_two_workspace_runtime(&terminal_stack, &workspace_list);
    workspace_switch(0, terminal_stack, workspace_list);

    target_ws = g_ptr_array_index(workspaces, 1);
    row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(workspace_list), 1);
    g_assert_nonnull(row);
    card = gtk_list_box_row_get_child(row);
    g_assert_nonnull(card);
    header_box = g_object_get_data(G_OBJECT(card), "header-box");
    g_assert_nonnull(header_box);
    rename_label = target_ws->sidebar_label;
    g_assert_true(GTK_IS_LABEL(rename_label));

    click = GTK_GESTURE_CLICK(g_object_get_data(G_OBJECT(header_box),
                                                "rename-click-controller"));
    g_assert_true(GTK_IS_GESTURE_CLICK(click));
    g_signal_emit_by_name(click, "pressed", 2, 0.0, 0.0);

    rename_entry = g_object_get_data(G_OBJECT(header_box), "rename-entry");
    g_assert_true(GTK_IS_ENTRY(rename_entry));
    gtk_editable_set_text(GTK_EDITABLE(rename_entry), "discard me");

    key_ctrl = g_object_get_data(G_OBJECT(rename_entry), "rename-key-controller");
    g_assert_true(GTK_IS_EVENT_CONTROLLER_KEY(key_ctrl));
    g_signal_emit_by_name(key_ctrl, "key-pressed",
                          GDK_KEY_Escape, 0u, (GdkModifierType)0, &handled);

    g_assert_true(handled);
    g_assert_null(g_object_get_data(G_OBJECT(header_box), "rename-entry"));
    g_assert_cmpstr(target_ws->name, ==, "Workspace 2");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(rename_label)), ==,
                    "Workspace 2");
    g_assert_cmpint(session_queue_save_call_count, ==, 0);
    g_assert_cmpint(current_workspace, ==, 0);

    reset_workspace_globals();
}

static void
test_workspace_tab_labels_preserve_command_titles_and_system_icons(void)
{
    GtkWidget *terminal_stack;
    GtkWidget *workspace_list;
    Workspace *ws;
    GtkNotebook *nb;
    GtkWidget *page0;
    GtkWidget *tab0;
    GtkWidget *label0;
    GtkWidget *terminal0;
    GtkWidget *page1;
    GtkWidget *tab1;
    GtkWidget *emoji1;
    GtkWidget *page2;
    GtkWidget *tab2;
    GtkWidget *emoji2;

    require_display_or_skip();
    reset_workspace_globals();

    init_two_workspace_runtime(&terminal_stack, &workspace_list);
    workspace_switch(0, terminal_stack, workspace_list);

    ws = g_ptr_array_index(workspaces, 0);
    nb = g_ptr_array_index(ws->pane_notebooks, 0);
    g_assert_true(GTK_IS_NOTEBOOK(nb));

    page0 = gtk_notebook_get_nth_page(nb, 0);
    tab0 = gtk_notebook_get_tab_label(nb, page0);
    label0 = tab_inner_label(tab0);
    terminal0 = g_object_get_data(G_OBJECT(page0), "linked-terminal");
    g_assert_true(GTK_IS_LABEL(label0));
    g_assert_true(GHOSTTY_IS_TERMINAL(terminal0));

    g_object_set_data_full(G_OBJECT(terminal0), "stub-title",
                           g_strdup("nvim README.md"), g_free);
    workspace_refresh_tab_labels(ws);
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(label0)), ==,
                    "nvim README.md");

    g_object_set_data_full(G_OBJECT(terminal0), "stub-title",
                           g_strdup("/etc/nginx"), g_free);
    workspace_refresh_tab_labels(ws);
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(label0)), ==,
                    ".../nginx");

    workspace_add_terminal_to_notebook_with_cwd(ws, nb, NULL, "/boot");
    page1 = gtk_notebook_get_nth_page(nb, 1);
    tab1 = gtk_notebook_get_tab_label(nb, page1);
    emoji1 = tab_emoji_label(tab1);
    g_assert_true(GTK_IS_LABEL(emoji1));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(emoji1)), ==, "🥾");

    workspace_add_terminal_to_notebook_with_cwd(ws, nb, NULL, "/bin");
    page2 = gtk_notebook_get_nth_page(nb, 2);
    tab2 = gtk_notebook_get_tab_label(nb, page2);
    emoji2 = tab_emoji_label(tab2);
    g_assert_true(GTK_IS_LABEL(emoji2));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(emoji2)), ==, "🧰");

    reset_workspace_globals();
}

static void
test_workspace_detect_git_callback_ignores_freed_workspace(void)
{
    Workspace *ws;

    require_display_or_skip();
    reset_workspace_globals();

    ws = alloc_workspace();
    ws->serial = 777;
    g_strlcpy(ws->cwd, "/tmp", sizeof(ws->cwd));

    workspaces = g_ptr_array_new();
    g_ptr_array_add(workspaces, ws);

    workspace_detect_git(ws);
    g_ptr_array_remove_index(workspaces, 0);
    free_workspace_fixture_without_async_cancel(ws);
    ws = NULL;

    for (int i = 0; i < 200; i++) {
        while (g_main_context_iteration(NULL, FALSE))
            ;
        g_usleep(1000);
    }

    g_ptr_array_unref(workspaces);
    workspaces = NULL;
}

static void
test_workspace_attach_to_instance_inserts_at_target_index(void)
{
    GtkWidget *terminal_stack;
    GtkWidget *workspace_list;
    Workspace *first;
    Workspace *second;
    Workspace *detached;
    GtkListBoxRow *row0;
    GtkWidget *card0;
    SidebarCtxDataMirror *ctx0;
    GtkListBoxRow *selected;

    require_display_or_skip();
    reset_workspace_globals();

    terminal_stack = gtk_stack_new();
    workspace_list = gtk_list_box_new();

    workspace_add(terminal_stack, workspace_list, NULL);
    workspace_add(terminal_stack, workspace_list, NULL);
    g_assert_cmpuint(workspaces->len, ==, 2);

    first = g_ptr_array_index(workspaces, 0);
    second = g_ptr_array_index(workspaces, 1);
    workspace_switch(0, terminal_stack, workspace_list);
    g_assert_cmpint(current_workspace, ==, 0);

    detached = workspace_detach_from_instance(1);
    g_assert_true(detached == second);
    g_assert_cmpuint(workspaces->len, ==, 1);

    g_assert_true(workspace_attach_to_instance(detached, 0));
    g_assert_cmpuint(workspaces->len, ==, 2);
    g_assert_true(g_ptr_array_index(workspaces, 0) == second);
    g_assert_true(g_ptr_array_index(workspaces, 1) == first);
    g_assert_cmpint(current_workspace, ==, 1);

    row0 = gtk_list_box_get_row_at_index(GTK_LIST_BOX(workspace_list), 0);
    g_assert_nonnull(row0);
    card0 = gtk_list_box_row_get_child(row0);
    g_assert_nonnull(card0);
    ctx0 = g_object_get_data(G_OBJECT(card0), "sidebar-ctx-data");
    g_assert_nonnull(ctx0);
    g_assert_true(ctx0->workspace == second);

    selected = gtk_list_box_get_selected_row(GTK_LIST_BOX(workspace_list));
    g_assert_nonnull(selected);
    g_assert_cmpint(gtk_list_box_row_get_index(selected), ==, 1);

    reset_workspace_globals();
}

static void
test_workspace_import_preserves_serial_on_collision(void)
{
    GtkWidget *terminal_stack;
    GtkWidget *workspace_list;
    Workspace *source;
    Workspace *other;
    Workspace *imported;
    guint64 source_serial_before;
    guint64 other_serial_before;
    g_autofree char *payload = NULL;
    g_autofree char *error = NULL;
    int imported_idx = -1;
    ghostty_app_t fake_app = (ghostty_app_t)(guintptr)0x1;

    require_display_or_skip();
    reset_workspace_globals();

    terminal_stack = gtk_stack_new();
    workspace_list = gtk_list_box_new();

    workspace_add(terminal_stack, workspace_list, fake_app);
    workspace_add(terminal_stack, workspace_list, fake_app);
    g_assert_cmpuint(workspaces->len, ==, 2);

    source = g_ptr_array_index(workspaces, 0);
    other = g_ptr_array_index(workspaces, 1);
    source_serial_before = source->serial;
    other_serial_before = other->serial;

    payload = g_strdup_printf(
        "{\"serial\":%" G_GUINT64_FORMAT
        ",\"name\":\"Moved Workspace\",\"notes\":\"\","
        "\"activePaneId\":\"pane-1\","
        "\"panes\":[{\"paneId\":\"pane-1\",\"activeTab\":0,"
        "\"tabs\":[{\"name\":\"Terminal\",\"customName\":false,"
        "\"cwd\":\"/tmp\"}]}]}",
        source_serial_before);
    g_assert_nonnull(payload);
    g_assert_null(error);

    g_assert_true(workspace_import_from_payload(payload, fake_app, &imported_idx,
                                                &error));
    g_assert_null(error);
    g_assert_cmpuint(workspaces->len, ==, 3);
    g_assert_cmpint(imported_idx, ==, 2);

    imported = g_ptr_array_index(workspaces, imported_idx);
    g_assert_cmpuint(imported->serial, ==, source_serial_before);
    g_assert_cmpuint(source->serial, !=, source_serial_before);
    g_assert_cmpuint(source->serial, !=, imported->serial);
    g_assert_cmpuint(other->serial, ==, other_serial_before);

    for (guint i = 0; i < workspaces->len; i++) {
        Workspace *left = g_ptr_array_index(workspaces, i);
        for (guint j = i + 1; j < workspaces->len; j++) {
            Workspace *right = g_ptr_array_index(workspaces, j);
            g_assert_cmpuint(left->serial, !=, right->serial);
        }
    }

    reset_workspace_globals();
}

/* ---- Phase 4 new behavior tests ---- */

static Workspace *
make_strip_workspace_with_vertical_split(int column_count)
{
    Workspace *ws = make_strip_workspace(column_count);

    GtkNotebook *new_nb = make_notebook_with_terminal("vsplit");
    g_ptr_array_add(ws->pane_notebooks, new_nb);
    g_ptr_array_add(ws->terminals, gtk_notebook_get_nth_page(new_nb, 0));

    workspace_strip_split_vertical_in_column(ws, GTK_WIDGET(new_nb));
    return ws;
}

static void
test_strip_vertical_split_pane_focus_up_down(void)
{
    require_display_or_skip();

    Workspace *ws = make_strip_workspace_with_vertical_split(1);
    WorkspaceColumn *col = g_ptr_array_index(ws->strip_state->columns, 0);
    GtkNotebook *top_pane;
    GtkNotebook *bottom_pane;

    g_assert_nonnull(col->panes);
    g_assert_cmpuint(col->panes->len, ==, 2);
    g_assert_cmpint(col->focused_pane, ==, 1);
    top_pane = g_ptr_array_index(col->panes, 0);
    bottom_pane = g_ptr_array_index(col->panes, 1);

    g_assert_true(workspace_strip_focus_pane_up(ws));
    g_assert_cmpint(col->focused_pane, ==, 0);
    g_assert_true(ws->active_pane == top_pane);

    g_assert_false(workspace_strip_focus_pane_up(ws));
    g_assert_cmpint(col->focused_pane, ==, 0);
    g_assert_true(ws->active_pane == top_pane);

    g_assert_true(workspace_strip_focus_pane_down(ws));
    g_assert_cmpint(col->focused_pane, ==, 1);
    g_assert_true(ws->active_pane == bottom_pane);

    g_assert_false(workspace_strip_focus_pane_down(ws));
    g_assert_cmpint(col->focused_pane, ==, 1);
    g_assert_true(ws->active_pane == bottom_pane);

    free_workspace_fixture(ws);
}

static void
test_strip_vertical_split_inserts_after_focused_pane(void)
{
    require_display_or_skip();

    Workspace *ws = make_strip_workspace_with_vertical_split(1);
    WorkspaceColumn *col = g_ptr_array_index(ws->strip_state->columns, 0);
    GtkNotebook *top_pane;
    GtkNotebook *bottom_pane;
    GtkNotebook *inserted_pane;

    g_assert_nonnull(col->panes);
    g_assert_cmpuint(col->panes->len, ==, 2);

    top_pane = g_ptr_array_index(col->panes, 0);
    bottom_pane = g_ptr_array_index(col->panes, 1);

    g_assert_true(workspace_strip_focus_pane_up(ws));
    g_assert_true(ws->active_pane == top_pane);
    g_assert_cmpint(col->focused_pane, ==, 0);

    g_assert_true(workspace_split_current_for_layout(ws,
                                                     GTK_ORIENTATION_VERTICAL,
                                                     NULL));

    g_assert_cmpuint(col->panes->len, ==, 3);
    inserted_pane = g_ptr_array_index(col->panes, 1);
    g_assert_true(g_ptr_array_index(col->panes, 0) == top_pane);
    g_assert_true(g_ptr_array_index(col->panes, 2) == bottom_pane);
    g_assert_true(GTK_IS_NOTEBOOK(inserted_pane));
    g_assert_true(inserted_pane != top_pane);
    g_assert_true(inserted_pane != bottom_pane);
    g_assert_cmpint(col->focused_pane, ==, 1);
    g_assert_true(ws->active_pane == inserted_pane);

    free_workspace_fixture(ws);
}

static void
test_strip_tab_switch_within_pane(void)
{
    require_display_or_skip();

    Workspace *ws = make_strip_workspace(1);
    GtkNotebook *nb = ws->active_pane;

    GtkWidget *term2 = ghostty_terminal_new(NULL);
    GtkWidget *tab2 = gtk_label_new("t1");
    gtk_notebook_append_page(nb, term2, tab2);
    g_ptr_array_add(ws->terminals, term2);
    g_assert_cmpint(gtk_notebook_get_n_pages(nb), ==, 2);

    gtk_notebook_set_current_page(nb, 0);
    g_assert_true(workspace_tab_next_for_layout(ws));
    g_assert_cmpint(gtk_notebook_get_current_page(nb), ==, 1);

    g_assert_true(workspace_tab_prev_for_layout(ws));
    g_assert_cmpint(gtk_notebook_get_current_page(nb), ==, 0);

    free_workspace_fixture(ws);
}

static void
test_strip_focus_left_right_navigates_columns(void)
{
    require_display_or_skip();

    Workspace *ws = make_strip_workspace(3);

    workspace_strip_focus_column(ws, 0);
    g_assert_cmpint(ws->strip_state->focused_col, ==, 0);

    g_assert_true(workspace_focus_next_for_layout(ws));
    g_assert_cmpint(ws->strip_state->focused_col, ==, 1);

    g_assert_true(workspace_focus_next_for_layout(ws));
    g_assert_cmpint(ws->strip_state->focused_col, ==, 2);

    g_assert_true(workspace_focus_prev_for_layout(ws));
    g_assert_cmpint(ws->strip_state->focused_col, ==, 1);

    g_assert_true(workspace_focus_prev_for_layout(ws));
    g_assert_cmpint(ws->strip_state->focused_col, ==, 0);

    free_workspace_fixture(ws);
}

static void
test_strip_focus_does_not_skip_columns(void)
{
    require_display_or_skip();

    Workspace *ws = make_strip_workspace(5);
    workspace_strip_focus_column(ws, 0);

    for (int i = 0; i < 5; i++) {
        g_assert_cmpint(ws->strip_state->focused_col, ==, i);
        if (i < 4)
            g_assert_true(workspace_focus_next_for_layout(ws));
    }

    for (int i = 4; i >= 0; i--) {
        g_assert_cmpint(ws->strip_state->focused_col, ==, i);
        if (i > 0)
            g_assert_true(workspace_focus_prev_for_layout(ws));
    }

    free_workspace_fixture(ws);
}

static void
test_strip_vertical_split_keeps_column_count(void)
{
    require_display_or_skip();

    Workspace *ws = make_strip_workspace(2);
    g_assert_cmpuint(ws->strip_state->columns->len, ==, 2);
    g_assert_cmpuint(ws->pane_notebooks->len, ==, 2);

    workspace_strip_focus_column(ws, 0);
    g_assert_true(workspace_split_current_for_layout(ws,
                                                     GTK_ORIENTATION_VERTICAL,
                                                     NULL));

    g_assert_cmpuint(ws->strip_state->columns->len, ==, 2);
    g_assert_cmpuint(ws->pane_notebooks->len, ==, 3);

    WorkspaceColumn *col0 = g_ptr_array_index(ws->strip_state->columns, 0);
    g_assert_cmpuint(col0->panes->len, ==, 2);

    WorkspaceColumn *col1 = g_ptr_array_index(ws->strip_state->columns, 1);
    g_assert_cmpuint(col1->panes->len, ==, 1);

    free_workspace_fixture(ws);
}

static void
test_strip_column_pane_count_api(void)
{
    require_display_or_skip();

    Workspace *ws = make_strip_workspace_with_vertical_split(2);

    g_assert_cmpint(workspace_strip_column_pane_count(ws, 0), ==, 2);
    g_assert_cmpint(workspace_strip_column_pane_count(ws, 1), ==, 1);

    free_workspace_fixture(ws);
}

static void
test_workspace_shutdown_terminals_requests_close_and_hangs_running_sessions(void)
{
    require_display_or_skip();
    reset_workspace_globals();

    workspaces = g_ptr_array_new();

    Workspace *ws0 = make_classic_workspace_with_tabs(2);
    Workspace *ws1 = make_classic_workspace_with_tabs(1);
    GtkWidget *term0 = g_ptr_array_index(ws0->terminals, 0);
    GtkWidget *term1 = g_ptr_array_index(ws0->terminals, 1);
    GtkWidget *term2 = g_ptr_array_index(ws1->terminals, 0);

    g_object_set_data(G_OBJECT(term0), "stub-session-id", GINT_TO_POINTER(101));
    g_object_set_data(G_OBJECT(term1), "stub-session-id", GINT_TO_POINTER(102));
    g_object_set_data(G_OBJECT(term1), "stub-process-exited", GINT_TO_POINTER(1));
    g_object_set_data(G_OBJECT(term2), "stub-session-id", GINT_TO_POINTER(0));

    g_ptr_array_add(workspaces, ws0);
    g_ptr_array_add(workspaces, ws1);

    workspace_shutdown_terminals();

    g_assert_cmpint(terminal_request_close_call_count, ==, 3);
    g_assert_cmpint(terminal_hangup_session_call_count, ==, 1);
    g_assert_true(GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(term0), "stub-close-requested")) != 0);
    g_assert_true(GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(term1), "stub-close-requested")) != 0);
    g_assert_true(GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(term2), "stub-close-requested")) != 0);
    g_assert_true(GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(term0), "stub-session-hung-up")) != 0);
    g_assert_false(GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(term1), "stub-session-hung-up")) != 0);
    g_assert_false(GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(term2), "stub-session-hung-up")) != 0);

    reset_workspace_globals();
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    have_display = gtk_init_check();

    g_test_add_func("/workspace-phase4/split/strip-horizontal",
                    test_split_current_for_layout_strip_horizontal_inserts_column);
    g_test_add_func("/workspace-phase4/split/strip-vertical-stacks-in-column",
                    test_split_current_for_layout_strip_vertical_stacks_in_column);

    g_test_add_func("/workspace-phase4/close/strip-removes-active-column",
                    test_close_current_for_layout_strip_removes_active_column);
    g_test_add_func("/workspace-phase4/close/strip-stacked-pane-keeps-column",
                    test_close_current_for_layout_strip_stacked_pane_keeps_column);
    g_test_add_func("/workspace-phase4/close/strip-single-column-fails",
                    test_close_current_for_layout_strip_single_column_fails);
    g_test_add_func("/workspace-phase4/shortcuts/strip-contract",
                    test_strip_shortcuts_match_phase4_contract);
    g_test_add_func("/workspace-phase4/close/classic-collapses-tree",
                    test_close_current_for_layout_classic_collapses_tree);

    g_test_add_func("/workspace-phase4/focus/strip-next-prev",
                    test_focus_next_prev_for_layout_strip_navigates_columns);
    g_test_add_func("/workspace-phase4/focus/strip-single-column-noop-success",
                    test_focus_next_prev_for_layout_strip_single_column_noop_success);
    g_test_add_func("/workspace-phase4/focus/classic-next-prev",
                    test_focus_next_prev_for_layout_classic_navigates_tabs);

    g_test_add_func("/workspace-phase4/split/strip-vertical-stacks-panes",
                    test_strip_vertical_split_keeps_column_count);
    g_test_add_func("/workspace-phase4/focus/strip-pane-up-down",
                    test_strip_vertical_split_pane_focus_up_down);
    g_test_add_func("/workspace-phase4/split/strip-vertical-after-focused-pane",
                    test_strip_vertical_split_inserts_after_focused_pane);
    g_test_add_func("/workspace-phase4/tab/strip-tab-switch-within-pane",
                    test_strip_tab_switch_within_pane);
    g_test_add_func("/workspace-phase4/focus/strip-left-right-navigates-columns",
                    test_strip_focus_left_right_navigates_columns);
    g_test_add_func("/workspace-phase4/focus/strip-does-not-skip-columns",
                    test_strip_focus_does_not_skip_columns);
    g_test_add_func("/workspace-phase4/strip/column-pane-count-api",
                    test_strip_column_pane_count_api);
    g_test_add_func("/workspace-phase7/status/sorted-order",
                    test_workspace_status_entries_sorted_attention_then_recency);
    g_test_add_func("/workspace-phase7/status/clear-entry",
                    test_workspace_status_entry_clear_removes_entry);
    g_test_add_func("/workspace-interactions/drag/move-tab-between-workspaces",
                    test_workspace_move_tab_moves_linked_terminal_between_workspaces);
    g_test_add_func("/workspace-interactions/selection/select-tab-switches-workspace",
                    test_workspace_select_tab_switches_target_workspace);
    g_test_add_func("/workspace-interactions/close/current-tab",
                    test_workspace_close_current_tab_closes_when_multiple_tabs);
    g_test_add_func("/workspace-interactions/delete/workspace-remove",
                    test_workspace_remove_deletes_sidebar_workspace_row);
    g_test_add_func("/workspace-interactions/card/drop-target-move-tab",
                    test_workspace_sidebar_card_drop_target_moves_tab_between_workspaces);
    g_test_add_func("/workspace-interactions/card/select-tab-switches-workspace",
                    test_workspace_sidebar_card_select_tab_switches_workspace);
    g_test_add_func("/workspace-interactions/card/delete-action-removes-workspace",
                    test_workspace_sidebar_card_delete_action_removes_workspace);
    g_test_add_func("/workspace-interactions/card/reorder-keeps-drop-controller",
                    test_workspace_sidebar_card_reorder_keeps_drop_controller_available);
    g_test_add_func("/workspace-interactions/card/rename-double-click-starts-inline",
                    test_workspace_sidebar_card_double_click_starts_inline_rename);
    g_test_add_func("/workspace-interactions/tab/rename-double-click-starts-inline",
                    test_workspace_terminal_tab_double_click_starts_inline_rename);
    g_test_add_func("/workspace-interactions/card/rename-focus-leave-commits",
                    test_workspace_sidebar_card_rename_focus_leave_commits_and_saves);
    g_test_add_func("/workspace-interactions/card/rename-escape-cancels",
                    test_workspace_sidebar_card_rename_escape_cancels_without_save);
    g_test_add_func("/workspace-interactions/tab/title-format-and-system-icons",
                    test_workspace_tab_labels_preserve_command_titles_and_system_icons);
    g_test_add_func("/workspace-regressions/git-detect/freed-workspace-callback",
                    test_workspace_detect_git_callback_ignores_freed_workspace);
    g_test_add_func("/workspace-phase10/attach/inserts-at-target-index",
                    test_workspace_attach_to_instance_inserts_at_target_index);
    g_test_add_func("/workspace-phase10/import/preserve-serial-on-collision",
                    test_workspace_import_preserves_serial_on_collision);
    g_test_add_func("/workspace-shutdown/terminals/request-close-and-hangup",
                    test_workspace_shutdown_terminals_requests_close_and_hangs_running_sessions);

    return g_test_run();
}
