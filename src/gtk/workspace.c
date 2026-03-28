#include "workspace.h"
#include "ghostty_terminal.h"
#include <stdio.h>
#include <string.h>

GPtrArray *workspaces = NULL;
int current_workspace = 0;

/* Idle callback: set a GtkPaned position to 50% of its allocated size. */
static gboolean set_paned_half(gpointer data) {
    GtkWidget *paned = GTK_WIDGET(data);
    if (!GTK_IS_PANED(paned)) { g_object_unref(paned); return G_SOURCE_REMOVE; }

    GtkOrientation orient = gtk_orientable_get_orientation(GTK_ORIENTABLE(paned));
    int size = (orient == GTK_ORIENTATION_HORIZONTAL)
        ? gtk_widget_get_width(paned)
        : gtk_widget_get_height(paned);

    if (size > 10)
        gtk_paned_set_position(GTK_PANED(paned), size / 2);
    else
        gtk_paned_set_position(GTK_PANED(paned), 200); /* fallback */

    g_object_unref(paned);
    return G_SOURCE_REMOVE;
}

/* Global widget references for DnD operations (set by workspace_add) */
GtkWidget *g_terminal_stack = NULL;
GtkWidget *g_workspace_list = NULL;

/* ── DnD data structure ─────────────────────────────────────────── */

/*
 * Drag payload: pointer to the terminal widget being dragged,
 * plus the source notebook and workspace index at drag start.
 */
typedef struct {
    GtkWidget *terminal;         /* GhosttyTerminal widget */
    GtkWidget *source_notebook;  /* GtkNotebook the tab was dragged from */
    int source_ws_idx;           /* Workspace index at drag start */
} TabDragData;

/* ── Forward declarations ───────────────────────────────────────── */

static GtkWidget *create_pane_notebook(Workspace *ws, ghostty_app_t app);
static void setup_tab_label_dnd(GtkWidget *label, GtkWidget *terminal,
                                GtkNotebook *notebook, Workspace *ws);
static void on_notebook_switch_page(GtkNotebook *nb, GtkWidget *page,
                                    guint page_num, gpointer user_data);
static void build_tab_label_text(GhosttyTerminal *term, const char *title,
                                 char *buf, size_t bufsz);

/* Context menu data for sidebar rows (defined later) */
typedef struct {
    Workspace *workspace;
    int ws_idx;
} SidebarCtxData;

static void on_sidebar_right_click(GtkGestureClick *gesture, int n_press,
                                   double x, double y, gpointer user_data);

/* ── Helpers ────────────────────────────────────────────────────── */

Workspace *workspace_get_current(void) {
    if (!workspaces || current_workspace >= (int)workspaces->len)
        return NULL;
    return g_ptr_array_index(workspaces, current_workspace);
}

static int workspace_index_of(Workspace *ws) {
    if (!workspaces) return -1;
    for (guint i = 0; i < workspaces->len; i++) {
        if (g_ptr_array_index(workspaces, i) == ws)
            return (int)i;
    }
    return -1;
}

/* ── Activity detection ────────────────────────────────────────── */

gboolean
workspace_has_activity(Workspace *ws)
{
    /* Stub: no activity tracking yet. */
    (void)ws;
    return FALSE;
}

/* ── Tab label refresh ─────────────────────────────────────────── */

void
workspace_refresh_tab_labels(Workspace *ws)
{
    /* Stub: tab labels are updated via title-changed signals. */
    (void)ws;
}

/* ── Sidebar label refresh ──────────────────────────────────────── */

void workspace_refresh_sidebar_label(Workspace *ws) {
    if (!ws || !ws->sidebar_label) return;
    char buf[512];
    gboolean has_act = workspace_has_activity(ws);

    /* Build the first line: [activity dot] name [branch] */
    char line1[256];
    if (ws->git_branch[0]) {
        if (has_act)
            snprintf(line1, sizeof(line1), "\342\227\217 %s [%s]",
                     ws->name, ws->git_branch);
        else
            snprintf(line1, sizeof(line1), "%s [%s]",
                     ws->name, ws->git_branch);
    } else {
        if (has_act)
            snprintf(line1, sizeof(line1), "\342\227\217 %s", ws->name);
        else
            snprintf(line1, sizeof(line1), "%s", ws->name);
    }

    /* Build optional second line: notification or port info */
    if (ws->notification[0]) {
        char *escaped_line1 = g_markup_escape_text(line1, -1);
        char *escaped_note  = g_markup_escape_text(ws->notification, -1);
        snprintf(buf, sizeof(buf),
                 "%s\n<small><i>%s</i></small>",
                 escaped_line1, escaped_note);
        g_free(escaped_line1);
        g_free(escaped_note);
        gtk_label_set_markup(GTK_LABEL(ws->sidebar_label), buf);
    } else {
        gtk_label_set_text(GTK_LABEL(ws->sidebar_label), line1);
    }

    /* Apply or remove the "has-activity" CSS class on the label */
    if (has_act)
        gtk_widget_add_css_class(ws->sidebar_label, "has-activity");
    else
        gtk_widget_remove_css_class(ws->sidebar_label, "has-activity");
}

/* ── Feature 2: Git branch detection (async) ────────────────────── */

static void
on_git_branch_read(GObject *source, GAsyncResult *result, gpointer user_data)
{
    Workspace *ws = user_data;
    char *stdout_buf = NULL;

    if (g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result,
                                             &stdout_buf, NULL, NULL)) {
        if (stdout_buf && stdout_buf[0]) {
            g_strstrip(stdout_buf);
            snprintf(ws->git_branch, sizeof(ws->git_branch), "%s", stdout_buf);
        } else {
            ws->git_branch[0] = '\0';
        }
        g_free(stdout_buf);
    } else {
        ws->git_branch[0] = '\0';
    }

    workspace_refresh_sidebar_label(ws);
}

void workspace_detect_git(Workspace *ws) {
    if (!ws || !ws->cwd[0]) {
        ws->git_branch[0] = '\0';
        workspace_refresh_sidebar_label(ws);
        return;
    }

    GError *error = NULL;
    GSubprocess *proc = g_subprocess_new(
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
        &error,
        "git", "-C", ws->cwd, "rev-parse", "--abbrev-ref", "HEAD", NULL);

    if (!proc) {
        if (error) g_error_free(error);
        ws->git_branch[0] = '\0';
        workspace_refresh_sidebar_label(ws);
        return;
    }

    g_subprocess_communicate_utf8_async(proc, NULL, NULL,
                                        on_git_branch_read, ws);
    g_object_unref(proc);
}

/* ── Tab title changed signal handler ───────────────────────────── */

static void
build_tab_label_text(GhosttyTerminal *term, const char *title, char *buf, size_t bufsz)
{
    char short_title[32];
    snprintf(short_title, sizeof(short_title), "%.28s", title ? title : "Terminal");

    /* Activity indicator (green dot prefix) */
    const char *activity_prefix = "";
    if (ghostty_terminal_has_activity(term))
        activity_prefix = "\342\227\217 ";   /* "● " in UTF-8 */

    /* Progress bar suffix */
    char progress_suffix[32];
    progress_suffix[0] = '\0';
    int pct = ghostty_terminal_get_progress_percent(term);
    int state = ghostty_terminal_get_progress_state(term);
    if (state > 0 && pct >= 0) {
        /* 5 blocks total: filled = pct/20, empty = 5-filled */
        int filled = pct / 20;
        if (filled > 5) filled = 5;
        int i;
        char bar[32];
        int pos = 0;
        for (i = 0; i < filled; i++) {
            /* U+25B0 = ▰ (3 bytes UTF-8: E2 96 B0) */
            bar[pos++] = (char)0xE2; bar[pos++] = (char)0x96; bar[pos++] = (char)0xB0;
        }
        for (i = filled; i < 5; i++) {
            /* U+25B1 = ▱ (3 bytes UTF-8: E2 96 B1) */
            bar[pos++] = (char)0xE2; bar[pos++] = (char)0x96; bar[pos++] = (char)0xB1;
        }
        bar[pos] = '\0';
        snprintf(progress_suffix, sizeof(progress_suffix), " %s %d%%", bar, pct);
    }

    snprintf(buf, bufsz, "%s%s%s", activity_prefix, short_title, progress_suffix);
}

static void on_title_changed(GhosttyTerminal *term, const char *title, gpointer label_ptr) {
    char buf[128];
    build_tab_label_text(term, title, buf, sizeof(buf));
    gtk_label_set_text(GTK_LABEL(label_ptr), buf);
}

/* ── Feature 4: Double-click to rename (tab labels + sidebar rows) ─ */

/*
 * When double-click is detected on a label, replace it with a GtkEntry
 * for inline editing.  On Enter or focus-out, restore the label.
 */

/* Data for the inline rename operation */
typedef struct {
    GtkWidget *event_box;        /* The parent box holding label or entry */
    GtkWidget *label;            /* The GtkLabel */
    GtkWidget *terminal;         /* Associated terminal (NULL for sidebar) */
    Workspace *workspace;        /* Associated workspace (for sidebar rows) */
    gboolean is_workspace_row;   /* TRUE if this is a workspace sidebar rename */
} RenameData;

static void
finish_rename(GtkEntry *entry, RenameData *rd)
{
    GtkEntryBuffer *buf = gtk_entry_get_buffer(GTK_ENTRY(entry));
    const char *new_text = gtk_entry_buffer_get_text(buf);
    if (new_text && new_text[0]) {
        gtk_label_set_text(GTK_LABEL(rd->label), new_text);
        if (rd->is_workspace_row && rd->workspace) {
            snprintf(rd->workspace->name, sizeof(rd->workspace->name),
                     "%.60s", new_text);
            workspace_refresh_sidebar_label(rd->workspace);
        }
    }

    /* Remove the entry and show the label again */
    GtkWidget *entry_widget = GTK_WIDGET(entry);
    GtkWidget *parent = rd->event_box;

    gtk_box_remove(GTK_BOX(parent), entry_widget);
    gtk_box_append(GTK_BOX(parent), rd->label);
    gtk_widget_set_visible(rd->label, TRUE);
}

static void
on_rename_entry_activate(GtkEntry *entry, gpointer user_data)
{
    RenameData *rd = user_data;
    finish_rename(entry, rd);
}

static void
on_rename_entry_focus_leave(GtkEventControllerFocus *ctrl, gpointer user_data)
{
    (void)ctrl;
    RenameData *rd = user_data;
    GtkWidget *entry_widget = gtk_event_controller_get_widget(
        GTK_EVENT_CONTROLLER(ctrl));
    if (GTK_IS_ENTRY(entry_widget))
        finish_rename(GTK_ENTRY(entry_widget), rd);
}

static void
on_label_double_click(GtkGestureClick *gesture, int n_press,
                      double x, double y, gpointer user_data)
{
    (void)gesture; (void)x; (void)y;
    if (n_press != 2) return;

    RenameData *rd = user_data;
    GtkWidget *parent = rd->event_box;
    const char *current_text = gtk_label_get_text(GTK_LABEL(rd->label));

    /* Hide label, insert entry */
    gtk_box_remove(GTK_BOX(parent), rd->label);

    GtkWidget *entry = gtk_entry_new();
    GtkEntryBuffer *buf = gtk_entry_get_buffer(GTK_ENTRY(entry));
    gtk_entry_buffer_set_text(buf, current_text, -1);
    gtk_widget_set_hexpand(entry, FALSE);
    gtk_widget_set_size_request(entry, 80, -1);

    g_signal_connect(entry, "activate",
                     G_CALLBACK(on_rename_entry_activate), rd);

    GtkEventController *focus_ctrl = gtk_event_controller_focus_new();
    g_signal_connect(focus_ctrl, "leave",
                     G_CALLBACK(on_rename_entry_focus_leave), rd);
    gtk_widget_add_controller(entry, focus_ctrl);

    gtk_box_append(GTK_BOX(parent), entry);
    gtk_widget_set_visible(entry, TRUE);
    gtk_widget_grab_focus(entry);
}

/*
 * Create a tab label widget with double-click-to-rename support.
 * Returns a GtkBox containing a GtkLabel with a gesture controller.
 * *out_label receives the inner GtkLabel pointer (for title-changed).
 */
static GtkWidget *
create_editable_tab_label(const char *text, GtkWidget *terminal,
                          Workspace *ws, gboolean is_workspace_row,
                          GtkWidget **out_label)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_box_append(GTK_BOX(box), label);

    RenameData *rd = g_new0(RenameData, 1);
    rd->event_box = box;
    rd->label = label;
    rd->terminal = terminal;
    rd->workspace = ws;
    rd->is_workspace_row = is_workspace_row;

    /* Prevent the RenameData from leaking */
    g_object_set_data_full(G_OBJECT(box), "rename-data", rd, g_free);

    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
    g_signal_connect(click, "pressed",
                     G_CALLBACK(on_label_double_click), rd);
    gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(click));

    if (out_label)
        *out_label = label;
    return box;
}

/* ── Feature 1: DnD - Tab drag source callbacks ─────────────────── */

static GdkContentProvider *
on_tab_drag_prepare(GtkDragSource *source, double x, double y,
                    gpointer user_data)
{
    (void)source; (void)x; (void)y;
    TabDragData *dd = user_data;

    GBytes *bytes = g_bytes_new(dd, sizeof(TabDragData));
    GdkContentProvider *provider = gdk_content_provider_new_typed(
        G_TYPE_BYTES, bytes);
    g_bytes_unref(bytes);
    return provider;
}

static void
on_tab_drag_begin(GtkDragSource *source, GdkDrag *drag, gpointer user_data)
{
    (void)user_data;
    GtkWidget *icon = gtk_label_new("Tab");
    gtk_widget_add_css_class(icon, "drag-icon");
    GdkPaintable *paintable = gtk_widget_paintable_new(icon);
    gdk_drag_set_hotspot(drag, 20, 10);
    gtk_drag_source_set_icon(source, paintable, 20, 10);
    g_object_unref(paintable);
}

/* ── Feature 1: DnD - Notebook drop target callbacks ────────────── */

static gboolean
on_notebook_drop(GtkDropTarget *target, const GValue *value,
                 double x, double y, gpointer user_data)
{
    (void)target; (void)x; (void)y;
    GtkNotebook *dest_nb = GTK_NOTEBOOK(user_data);

    if (!G_VALUE_HOLDS(value, G_TYPE_BYTES))
        return FALSE;

    GBytes *bytes = g_value_get_boxed(value);
    if (g_bytes_get_size(bytes) != sizeof(TabDragData))
        return FALSE;

    const TabDragData *dd = g_bytes_get_data(bytes, NULL);
    GtkWidget *terminal = dd->terminal;
    GtkNotebook *src_nb = GTK_NOTEBOOK(dd->source_notebook);

    if (src_nb == dest_nb)
        return FALSE;

    /* Find the tab page index in the source notebook */
    int src_page = -1;
    int n_pages = gtk_notebook_get_n_pages(src_nb);
    int i;
    for (i = 0; i < n_pages; i++) {
        if (gtk_notebook_get_nth_page(src_nb, i) == terminal) {
            src_page = i;
            break;
        }
    }
    if (src_page < 0)
        return FALSE;

    /* Get the tab label text before removal */
    GtkWidget *old_tab_widget = gtk_notebook_get_tab_label(src_nb, terminal);
    const char *tab_text = "Terminal";
    if (old_tab_widget) {
        GtkWidget *inner = gtk_widget_get_first_child(old_tab_widget);
        if (GTK_IS_LABEL(inner))
            tab_text = gtk_label_get_text(GTK_LABEL(inner));
    }
    char saved_text[64];
    snprintf(saved_text, sizeof(saved_text), "%s", tab_text);

    /* Remove terminal from source workspace's terminals list */
    Workspace *src_ws = NULL;
    if (dd->source_ws_idx >= 0 && dd->source_ws_idx < (int)workspaces->len)
        src_ws = g_ptr_array_index(workspaces, dd->source_ws_idx);
    if (src_ws)
        g_ptr_array_remove(src_ws->terminals, terminal);

    /* Ref the terminal so it survives reparenting */
    g_object_ref(terminal);
    gtk_notebook_remove_page(src_nb, src_page);

    /* Find the destination workspace */
    Workspace *dest_ws = NULL;
    guint wi;
    for (wi = 0; wi < workspaces->len; wi++) {
        Workspace *ws = g_ptr_array_index(workspaces, wi);
        guint pi;
        for (pi = 0; pi < ws->pane_notebooks->len; pi++) {
            if (g_ptr_array_index(ws->pane_notebooks, pi) == dest_nb) {
                dest_ws = ws;
                break;
            }
        }
        if (dest_ws) break;
    }

    /* Create new tab label for destination */
    GtkWidget *new_label = NULL;
    GtkWidget *tab_label_widget = create_editable_tab_label(
        saved_text, terminal, dest_ws, FALSE, &new_label);

    gtk_notebook_append_page(dest_nb, terminal, tab_label_widget);
    gtk_notebook_set_tab_reorderable(dest_nb, terminal, TRUE);

    if (dest_ws)
        g_ptr_array_add(dest_ws->terminals, terminal);

    /* Set up DnD on the new tab label */
    if (dest_ws)
        setup_tab_label_dnd(tab_label_widget, terminal, dest_nb, dest_ws);

    /* Connect title-changed to the new label */
    if (new_label && GHOSTTY_IS_TERMINAL(terminal))
        g_signal_connect(terminal, "title-changed",
                         G_CALLBACK(on_title_changed), new_label);

    g_object_unref(terminal);

    gtk_notebook_set_current_page(dest_nb,
        gtk_notebook_get_n_pages(dest_nb) - 1);

    return TRUE;
}

/* ── Feature 1: DnD - Workspace sidebar drop target callbacks ───── */

static gboolean
on_ws_sidebar_drop(GtkDropTarget *target, const GValue *value,
                   double x, double y, gpointer user_data)
{
    (void)target; (void)x; (void)y;
    int dest_ws_idx = GPOINTER_TO_INT(user_data);

    if (!G_VALUE_HOLDS(value, G_TYPE_BYTES))
        return FALSE;

    GBytes *bytes = g_value_get_boxed(value);
    if (g_bytes_get_size(bytes) != sizeof(TabDragData))
        return FALSE;

    const TabDragData *dd = g_bytes_get_data(bytes, NULL);
    GtkWidget *terminal = dd->terminal;
    GtkNotebook *src_nb = GTK_NOTEBOOK(dd->source_notebook);

    if (dest_ws_idx < 0 || dest_ws_idx >= (int)workspaces->len)
        return FALSE;

    Workspace *dest_ws = g_ptr_array_index(workspaces, dest_ws_idx);

    /* Don't drop on same workspace if it only has one notebook */
    Workspace *src_ws = NULL;
    if (dd->source_ws_idx >= 0 && dd->source_ws_idx < (int)workspaces->len)
        src_ws = g_ptr_array_index(workspaces, dd->source_ws_idx);
    if (src_ws == dest_ws && dest_ws->pane_notebooks->len == 1)
        return FALSE;

    /* Find tab in source notebook */
    int src_page = -1;
    int n_pages = gtk_notebook_get_n_pages(src_nb);
    int i;
    for (i = 0; i < n_pages; i++) {
        if (gtk_notebook_get_nth_page(src_nb, i) == terminal) {
            src_page = i;
            break;
        }
    }
    if (src_page < 0)
        return FALSE;

    /* Save tab text */
    GtkWidget *old_tab_widget = gtk_notebook_get_tab_label(src_nb, terminal);
    const char *tab_text = "Terminal";
    if (old_tab_widget) {
        GtkWidget *inner = gtk_widget_get_first_child(old_tab_widget);
        if (GTK_IS_LABEL(inner))
            tab_text = gtk_label_get_text(GTK_LABEL(inner));
    }
    char saved_text[64];
    snprintf(saved_text, sizeof(saved_text), "%s", tab_text);

    /* Remove from source */
    if (src_ws)
        g_ptr_array_remove(src_ws->terminals, terminal);

    g_object_ref(terminal);
    gtk_notebook_remove_page(src_nb, src_page);

    /* Add to destination workspace's first notebook */
    GtkNotebook *dest_nb = GTK_NOTEBOOK(dest_ws->notebook);

    GtkWidget *new_label = NULL;
    GtkWidget *tab_label_widget = create_editable_tab_label(
        saved_text, terminal, dest_ws, FALSE, &new_label);

    gtk_notebook_append_page(dest_nb, terminal, tab_label_widget);
    gtk_notebook_set_tab_reorderable(dest_nb, terminal, TRUE);
    g_ptr_array_add(dest_ws->terminals, terminal);

    setup_tab_label_dnd(tab_label_widget, terminal, dest_nb, dest_ws);

    if (new_label && GHOSTTY_IS_TERMINAL(terminal))
        g_signal_connect(terminal, "title-changed",
                         G_CALLBACK(on_title_changed), new_label);

    g_object_unref(terminal);

    /* Switch to dest workspace */
    if (g_terminal_stack && g_workspace_list)
        workspace_switch(dest_ws_idx, g_terminal_stack, g_workspace_list);

    gtk_notebook_set_current_page(dest_nb,
        gtk_notebook_get_n_pages(dest_nb) - 1);

    return TRUE;
}

/* ── DnD: Setup drag source on tab labels ───────────────────────── */

static void
setup_tab_label_dnd(GtkWidget *label_widget, GtkWidget *terminal,
                    GtkNotebook *notebook, Workspace *ws)
{
    TabDragData *dd = g_new0(TabDragData, 1);
    dd->terminal = terminal;
    dd->source_notebook = GTK_WIDGET(notebook);
    dd->source_ws_idx = workspace_index_of(ws);

    /* Prevent the TabDragData from leaking */
    g_object_set_data_full(G_OBJECT(label_widget), "tab-drag-data", dd, g_free);

    GtkDragSource *drag_source = gtk_drag_source_new();
    gtk_drag_source_set_actions(drag_source, GDK_ACTION_MOVE);
    g_signal_connect(drag_source, "prepare",
                     G_CALLBACK(on_tab_drag_prepare), dd);
    g_signal_connect(drag_source, "drag-begin",
                     G_CALLBACK(on_tab_drag_begin), dd);
    gtk_widget_add_controller(label_widget, GTK_EVENT_CONTROLLER(drag_source));
}

/* ── DnD: Setup drop target on pane notebooks ───────────────────── */

static void
setup_notebook_drop_target(GtkNotebook *notebook)
{
    GtkDropTarget *drop = gtk_drop_target_new(G_TYPE_BYTES, GDK_ACTION_MOVE);
    g_signal_connect(drop, "drop",
                     G_CALLBACK(on_notebook_drop), notebook);
    gtk_widget_add_controller(GTK_WIDGET(notebook), GTK_EVENT_CONTROLLER(drop));
}

/* ── DnD: Setup drop target on workspace sidebar rows ───────────── */

static void
setup_ws_sidebar_drop_target(GtkWidget *row_widget, int ws_idx)
{
    GtkDropTarget *drop = gtk_drop_target_new(G_TYPE_BYTES, GDK_ACTION_MOVE);
    g_signal_connect(drop, "drop",
                     G_CALLBACK(on_ws_sidebar_drop), GINT_TO_POINTER(ws_idx));
    gtk_widget_add_controller(row_widget, GTK_EVENT_CONTROLLER(drop));
}

/* ── Add terminal to notebook ───────────────────────────────────── */

static void
workspace_add_terminal_to_notebook(Workspace *ws, GtkNotebook *notebook,
                                   ghostty_app_t app)
{
    (void)app;
    GtkWidget *terminal = ghostty_terminal_new(NULL);
    g_ptr_array_add(ws->terminals, terminal);

    GtkWidget *inner_label = NULL;
    GtkWidget *tab_label = create_editable_tab_label(
        "Terminal", terminal, ws, FALSE, &inner_label);

    gtk_notebook_append_page(notebook, terminal, tab_label);
    gtk_notebook_set_tab_reorderable(notebook, terminal, TRUE);

    /* Connect title-changed to update the inner label */
    g_signal_connect(terminal, "title-changed",
                     G_CALLBACK(on_title_changed), inner_label);

    /* Set up DnD on the tab label */
    setup_tab_label_dnd(tab_label, terminal, notebook, ws);

    gtk_widget_set_visible(terminal, TRUE);
    gtk_notebook_set_current_page(notebook,
        gtk_notebook_get_n_pages(notebook) - 1);
}

void workspace_add_terminal(Workspace *ws, ghostty_app_t app) {
    /* Add to the first pane notebook (backwards compat). */
    workspace_add_terminal_to_notebook(ws, GTK_NOTEBOOK(ws->notebook), app);
}

void workspace_add_terminal_to_focused(Workspace *ws, ghostty_app_t app) {
    GtkNotebook *focused = workspace_get_focused_pane(ws);
    if (focused)
        workspace_add_terminal_to_notebook(ws, focused, app);
    else
        workspace_add_terminal(ws, app);
}

void workspace_add_terminal_to_notebook_external(Workspace *ws,
                                                  GtkNotebook *notebook,
                                                  ghostty_app_t app) {
    workspace_add_terminal_to_notebook(ws, notebook, app);
}

/* ── Workspace sidebar row ──────────────────────────────────────── */

static GtkWidget *create_workspace_row(Workspace *ws, int ws_idx) {
    GtkWidget *inner_label = NULL;
    GtkWidget *box = create_editable_tab_label(
        ws->name, NULL, ws, TRUE, &inner_label);
    gtk_widget_add_css_class(box, "sidebar-row");
    ws->sidebar_label = inner_label;

    /* Set up drop target for workspace DnD */
    setup_ws_sidebar_drop_target(box, ws_idx);

    /* Right-click context menu (Rename / Delete) */
    {
        SidebarCtxData *ctx = g_new0(SidebarCtxData, 1);
        ctx->workspace = ws;
        ctx->ws_idx = ws_idx;
        g_object_set_data_full(G_OBJECT(box), "sidebar-ctx-data", ctx, g_free);

        GtkGesture *rclick = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(rclick), GDK_BUTTON_SECONDARY);
        g_signal_connect(rclick, "pressed",
                         G_CALLBACK(on_sidebar_right_click), ctx);
        gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(rclick));
    }

    return box;
}

/* ── "+" button callback ────────────────────────────────────────── */

static void on_ws_add_tab_clicked(GtkButton *btn, gpointer data) {
    (void)data;
    Workspace *w = g_object_get_data(G_OBJECT(btn), "workspace");
    ghostty_app_t a = g_object_get_data(G_OBJECT(btn), "app");
    workspace_add_terminal(w, a);
}

/* Helper: create a notebook for a new pane and wire up the "+" button. */
static GtkWidget *
create_pane_notebook(Workspace *ws, ghostty_app_t app)
{
    GtkWidget *notebook = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(notebook), TRUE);
    gtk_notebook_set_show_border(GTK_NOTEBOOK(notebook), FALSE);

    /* "+" button */
    GtkWidget *add_btn = gtk_button_new_with_label("+");
    g_object_set_data(G_OBJECT(add_btn), "workspace", ws);
    g_object_set_data(G_OBJECT(add_btn), "app", app);
    g_signal_connect(add_btn, "clicked", G_CALLBACK(on_ws_add_tab_clicked), NULL);
    gtk_notebook_set_action_widget(GTK_NOTEBOOK(notebook), add_btn, GTK_PACK_END);
    gtk_widget_set_visible(add_btn, TRUE);

    /* Set up drop target so tabs can be dropped onto this notebook */
    setup_notebook_drop_target(GTK_NOTEBOOK(notebook));

    /* On tab switch, clear activity on the newly selected terminal */
    g_signal_connect(notebook, "switch-page",
                     G_CALLBACK(on_notebook_switch_page), ws);

    return notebook;
}

/* ── Workspace add/remove/switch ────────────────────────────────── */

void workspace_add(GtkWidget *terminal_stack, GtkWidget *workspace_list, ghostty_app_t app) {
    if (!workspaces)
        workspaces = g_ptr_array_new();

    /* Store global references for DnD */
    g_terminal_stack = terminal_stack;
    g_workspace_list = workspace_list;

    Workspace *ws = g_new0(Workspace, 1);
    snprintf(ws->name, sizeof(ws->name), "Workspace %d", (int)workspaces->len + 1);
    ws->terminals = g_ptr_array_new();
    ws->pane_notebooks = g_ptr_array_new();
    ws->sidebar_label = NULL;

    /* Create the first pane notebook */
    ws->notebook = create_pane_notebook(ws, app);
    g_ptr_array_add(ws->pane_notebooks, ws->notebook);

    ws->container = ws->notebook;

    /* Add to stack */
    char stack_name[32];
    snprintf(stack_name, sizeof(stack_name), "ws-%d", (int)workspaces->len);
    gtk_stack_add_named(GTK_STACK(terminal_stack), ws->container, stack_name);

    /* Add to sidebar */
    int ws_idx = (int)workspaces->len;
    GtkWidget *row = create_workspace_row(ws, ws_idx);
    gtk_list_box_append(GTK_LIST_BOX(workspace_list), row);

    g_ptr_array_add(workspaces, ws);

    /* Create first terminal */
    workspace_add_terminal(ws, app);

    /* Switch to it */
    workspace_switch(workspaces->len - 1, terminal_stack, workspace_list);
}

void workspace_remove(int index, GtkWidget *terminal_stack, GtkWidget *workspace_list) {
    if (!workspaces || workspaces->len <= 1 || index >= (int)workspaces->len)
        return;

    Workspace *ws = g_ptr_array_index(workspaces, index);
    gtk_stack_remove(GTK_STACK(terminal_stack), ws->container);

    GtkListBoxRow *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(workspace_list), index);
    if (row) gtk_list_box_remove(GTK_LIST_BOX(workspace_list), GTK_WIDGET(row));

    g_ptr_array_remove_index(workspaces, index);
    if (current_workspace >= (int)workspaces->len)
        current_workspace = workspaces->len - 1;

    workspace_switch(current_workspace, terminal_stack, workspace_list);

    g_ptr_array_unref(ws->terminals);
    g_ptr_array_unref(ws->pane_notebooks);
    g_free(ws->notes_text);
    g_free(ws);
}

void workspace_switch(int index, GtkWidget *terminal_stack, GtkWidget *workspace_list) {
    if (!workspaces || index < 0 || index >= (int)workspaces->len)
        return;
    current_workspace = index;

    char stack_name[32];
    snprintf(stack_name, sizeof(stack_name), "ws-%d", index);
    gtk_stack_set_visible_child_name(GTK_STACK(terminal_stack), stack_name);

    GtkListBoxRow *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(workspace_list), index);
    if (row)
        gtk_list_box_select_row(GTK_LIST_BOX(workspace_list), row);
}

/* ── Pane splitting ───────────────────────────────────────────── */

/*
 * workspace_get_focused_pane:
 *
 * Walk the workspace's pane notebooks and find which one contains the
 * widget that currently has keyboard focus.  Falls back to the first
 * notebook.
 */
GtkNotebook *
workspace_get_focused_pane(Workspace *ws)
{
    if (!ws || !ws->pane_notebooks || ws->pane_notebooks->len == 0)
        return ws ? GTK_NOTEBOOK(ws->notebook) : NULL;

    GtkNotebook *first_nb = g_ptr_array_index(ws->pane_notebooks, 0);

    /* Try to find the focused widget */
    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(first_nb));
    GtkWidget *focus = root ? gtk_root_get_focus(root) : NULL;

    if (focus) {
        GtkWidget *w;
        for (w = focus; w != NULL; w = gtk_widget_get_parent(w)) {
            if (GTK_IS_NOTEBOOK(w)) {
                guint i;
                for (i = 0; i < ws->pane_notebooks->len; i++) {
                    if (g_ptr_array_index(ws->pane_notebooks, i) == w)
                        return GTK_NOTEBOOK(w);
                }
            }
        }
    }

    return first_nb;
}

/*
 * workspace_split_pane:
 *
 * Splits the currently focused pane notebook.  The notebook is
 * replaced in its parent by a GtkPaned; the original notebook
 * becomes start_child and a new notebook becomes end_child.
 *
 * If the notebook is the direct workspace container (no splits
 * yet), we replace ws->container in the GtkStack.
 */
void
workspace_split_pane(Workspace *ws, GtkOrientation orientation,
                     ghostty_app_t app)
{
    if (!ws) return;

    GtkNotebook *source_nb = workspace_get_focused_pane(ws);
    if (!source_nb) return;

    GtkWidget *source_widget = GTK_WIDGET(source_nb);
    GtkWidget *parent = gtk_widget_get_parent(source_widget);

    /* Create the new pane notebook */
    GtkWidget *new_nb = create_pane_notebook(ws, app);
    g_ptr_array_add(ws->pane_notebooks, new_nb);

    /* Create the new paned container */
    GtkWidget *paned = gtk_paned_new(orientation);
    gtk_widget_set_hexpand(paned, TRUE);
    gtk_widget_set_vexpand(paned, TRUE);

    if (GTK_IS_PANED(parent)) {
        GtkWidget *start = gtk_paned_get_start_child(GTK_PANED(parent));

        if (start == source_widget) {
            gtk_paned_set_start_child(GTK_PANED(parent), NULL);
            gtk_paned_set_start_child(GTK_PANED(paned), source_widget);
            gtk_paned_set_end_child(GTK_PANED(paned), new_nb);
            gtk_paned_set_start_child(GTK_PANED(parent), paned);
        } else {
            gtk_paned_set_end_child(GTK_PANED(parent), NULL);
            gtk_paned_set_start_child(GTK_PANED(paned), source_widget);
            gtk_paned_set_end_child(GTK_PANED(paned), new_nb);
            gtk_paned_set_end_child(GTK_PANED(parent), paned);
        }

        gtk_paned_set_resize_start_child(GTK_PANED(paned), TRUE);
        gtk_paned_set_resize_end_child(GTK_PANED(paned), TRUE);

    } else {
        GtkWidget *stack = parent;
        int ws_idx = workspace_index_of(ws);

        g_object_ref(source_widget);
        gtk_stack_remove(GTK_STACK(stack), source_widget);

        gtk_paned_set_start_child(GTK_PANED(paned), source_widget);
        gtk_paned_set_end_child(GTK_PANED(paned), new_nb);
        gtk_paned_set_resize_start_child(GTK_PANED(paned), TRUE);
        gtk_paned_set_resize_end_child(GTK_PANED(paned), TRUE);

        g_object_unref(source_widget);

        char stack_name[32];
        snprintf(stack_name, sizeof(stack_name), "ws-%d",
                 ws_idx >= 0 ? ws_idx : 0);
        gtk_stack_add_named(GTK_STACK(stack), paned, stack_name);
        gtk_stack_set_visible_child(GTK_STACK(stack), paned);

        ws->container = paned;
    }

    gtk_widget_set_visible(paned, TRUE);
    gtk_widget_set_visible(new_nb, TRUE);

    /* Set paned to 50% once it has a size */
    g_object_ref(paned);
    g_idle_add(set_paned_half, paned);

    /* Add a terminal to the new pane */
    workspace_add_terminal_to_notebook(ws, GTK_NOTEBOOK(new_nb), app);

    /* Focus the new terminal's GtkGLArea so it receives key events */
    {
        int last = gtk_notebook_get_n_pages(GTK_NOTEBOOK(new_nb)) - 1;
        if (last >= 0) {
            GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(new_nb), last);
            if (page) {
                GtkWidget *child = gtk_widget_get_first_child(page);
                if (child)
                    gtk_widget_grab_focus(child);
                else
                    gtk_widget_grab_focus(page);
            }
        }
    }
}

/* ── Pane zoom ───────────────────────────────────────────────── */

void
workspace_toggle_zoom(Workspace *ws)
{
    if (!ws) return;

    if (ws->zoomed) {
        /* Un-zoom: show all pane notebooks */
        if (ws->pane_notebooks) {
            guint i;
            for (i = 0; i < ws->pane_notebooks->len; i++) {
                GtkWidget *nb = g_ptr_array_index(ws->pane_notebooks, i);
                gtk_widget_set_visible(nb, TRUE);
            }
        }
        ws->zoomed = FALSE;
        ws->zoomed_pane = NULL;
        return;
    }

    /* Only zoom if there are multiple panes */
    if (!ws->pane_notebooks || ws->pane_notebooks->len <= 1)
        return;

    /* Find the focused pane */
    GtkNotebook *focused = workspace_get_focused_pane(ws);
    if (!focused)
        return;

    /* Hide all other pane notebooks */
    {
        guint i;
        for (i = 0; i < ws->pane_notebooks->len; i++) {
            GtkNotebook *nb = g_ptr_array_index(ws->pane_notebooks, i);
            if (nb != focused)
                gtk_widget_set_visible(GTK_WIDGET(nb), FALSE);
        }
    }

    ws->zoomed = TRUE;
    ws->zoomed_pane = focused;
}

/* ── Notes panel ─────────────────────────────────────────────── */

void
workspace_save_notes(Workspace *ws)
{
    if (!ws || !ws->notes_text)
        return;

    /* Notes text is saved on toggle-hide; this is a no-op placeholder
     * for external callers that want to ensure notes are persisted. */
}

void
workspace_restore_notes(Workspace *ws)
{
    (void)ws;
    /* Notes text is restored on toggle-show; no-op placeholder. */
}

void
workspace_toggle_notes(Workspace *ws, GtkWidget *notes_container)
{
    if (!ws || !notes_container)
        return;

    /* Look for an existing notes panel child in the container. */
    GtkWidget *child = gtk_widget_get_first_child(notes_container);
    GtkWidget *notes_panel = NULL;
    while (child) {
        const char *name = gtk_widget_get_name(child);
        if (name && strcmp(name, "workspace-notes-panel") == 0) {
            notes_panel = child;
            break;
        }
        child = gtk_widget_get_next_sibling(child);
    }

    if (notes_panel) {
        /* Already visible -- save and hide */
        GtkTextBuffer *buf = gtk_text_view_get_buffer(
            GTK_TEXT_VIEW(gtk_scrolled_window_get_child(
                GTK_SCROLLED_WINDOW(notes_panel))));
        GtkTextIter start_iter, end_iter;
        gtk_text_buffer_get_bounds(buf, &start_iter, &end_iter);
        char *text = gtk_text_buffer_get_text(buf, &start_iter, &end_iter, FALSE);
        g_free(ws->notes_text);
        ws->notes_text = text; /* takes ownership */

        if (GTK_IS_BOX(notes_container))
            gtk_box_remove(GTK_BOX(notes_container), notes_panel);
        else if (GTK_IS_PANED(notes_container))
            gtk_widget_set_visible(notes_panel, FALSE);
        return;
    }

    /* Create notes panel */
    GtkWidget *text_view = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text_view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(text_view), 8);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(text_view), 8);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(text_view), 4);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(text_view), 4);

    /* Restore saved text */
    if (ws->notes_text && ws->notes_text[0]) {
        GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_view));
        gtk_text_buffer_set_text(buf, ws->notes_text, -1);
    }

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), text_view);
    gtk_widget_set_size_request(scroll, -1, 120);
    gtk_widget_set_name(scroll, "workspace-notes-panel");

    if (GTK_IS_BOX(notes_container)) {
        gtk_box_append(GTK_BOX(notes_container), scroll);
    }

    gtk_widget_set_visible(scroll, TRUE);
    gtk_widget_grab_focus(text_view);
}

/* ── Pane navigation ─────────────────────────────────────────── */

/*
 * workspace_navigate_pane:
 *
 * Find the pane that is geometrically in the direction (dx, dy)
 * from the currently focused pane and give it focus.  Uses
 * graphene_rect_t (available via GTK4) from compute_bounds to
 * get positions.
 */
void
workspace_navigate_pane(Workspace *ws, int dx, int dy)
{
    if (!ws || !ws->pane_notebooks || ws->pane_notebooks->len <= 1)
        return;

    GtkNotebook *focused = workspace_get_focused_pane(ws);
    if (!focused)
        return;

    /* Get the position of the focused pane */
    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(focused));
    if (!root)
        return;

    graphene_point_t focused_pos;
    if (!gtk_widget_compute_point(GTK_WIDGET(focused), GTK_WIDGET(root),
                                  &GRAPHENE_POINT_INIT(0, 0), &focused_pos))
        return;

    double focused_w = (double)gtk_widget_get_width(GTK_WIDGET(focused));
    double focused_h = (double)gtk_widget_get_height(GTK_WIDGET(focused));
    double focused_cx = focused_pos.x + focused_w / 2.0;
    double focused_cy = focused_pos.y + focused_h / 2.0;

    GtkNotebook *best = NULL;
    double best_dist = 1e18;
    guint i;

    for (i = 0; i < ws->pane_notebooks->len; i++) {
        GtkNotebook *nb = g_ptr_array_index(ws->pane_notebooks, i);
        if (nb == focused)
            continue;

        graphene_point_t nb_pos;
        if (!gtk_widget_compute_point(GTK_WIDGET(nb), GTK_WIDGET(root),
                                      &GRAPHENE_POINT_INIT(0, 0), &nb_pos))
            continue;

        double nb_w = (double)gtk_widget_get_width(GTK_WIDGET(nb));
        double nb_h = (double)gtk_widget_get_height(GTK_WIDGET(nb));
        double nb_cx = nb_pos.x + nb_w / 2.0;
        double nb_cy = nb_pos.y + nb_h / 2.0;

        double ddx = nb_cx - focused_cx;
        double ddy = nb_cy - focused_cy;

        /* Filter: candidate must be in the requested direction */
        if (dx > 0 && ddx <= 0) continue;
        if (dx < 0 && ddx >= 0) continue;
        if (dy > 0 && ddy <= 0) continue;
        if (dy < 0 && ddy >= 0) continue;

        double dist = ddx * ddx + ddy * ddy;
        if (dist < best_dist) {
            best_dist = dist;
            best = nb;
        }
    }

    if (!best)
        return;

    /* Focus the current page's first focusable child in the target pane */
    int pg = gtk_notebook_get_current_page(best);
    if (pg >= 0) {
        GtkWidget *page = gtk_notebook_get_nth_page(best, pg);
        if (page) {
            GtkWidget *child = gtk_widget_get_first_child(page);
            if (child)
                gtk_widget_grab_focus(child);
            else
                gtk_widget_grab_focus(page);
        }
    }
}

/* ── Close pane ──────────────────────────────────────────────── */

void
workspace_close_pane(Workspace *ws, GtkNotebook *pane)
{
    if (!ws || !pane) return;
    if (!ws->pane_notebooks || ws->pane_notebooks->len <= 1) return;

    GtkWidget *pane_widget = GTK_WIDGET(pane);
    GtkWidget *parent = gtk_widget_get_parent(pane_widget);

    if (!GTK_IS_PANED(parent)) return;

    GtkPaned *parent_paned = GTK_PANED(parent);
    GtkWidget *grandparent = gtk_widget_get_parent(GTK_WIDGET(parent_paned));

    /* Determine the sibling (the other child of the paned). */
    GtkWidget *start = gtk_paned_get_start_child(parent_paned);
    GtkWidget *sibling = (start == pane_widget)
        ? gtk_paned_get_end_child(parent_paned)
        : start;

    if (!sibling) return;

    /* Remove terminals that belong to the closing pane from ws->terminals */
    {
        int n_pages = gtk_notebook_get_n_pages(pane);
        int i;
        for (i = 0; i < n_pages; i++) {
            GtkWidget *child = gtk_notebook_get_nth_page(pane, i);
            g_ptr_array_remove(ws->terminals, child);
        }
    }

    /* Remove the pane from pane_notebooks */
    g_ptr_array_remove(ws->pane_notebooks, pane);

    /* Unparent both children from the paned */
    g_object_ref(sibling);
    gtk_paned_set_start_child(parent_paned, NULL);
    gtk_paned_set_end_child(parent_paned, NULL);

    /* Replace the paned with the sibling in its grandparent */
    if (GTK_IS_PANED(grandparent)) {
        GtkWidget *gp_start = gtk_paned_get_start_child(GTK_PANED(grandparent));
        if (gp_start == GTK_WIDGET(parent_paned)) {
            gtk_paned_set_start_child(GTK_PANED(grandparent), NULL);
            gtk_paned_set_start_child(GTK_PANED(grandparent), sibling);
        } else {
            gtk_paned_set_end_child(GTK_PANED(grandparent), NULL);
            gtk_paned_set_end_child(GTK_PANED(grandparent), sibling);
        }
    } else if (GTK_IS_STACK(grandparent)) {
        int ws_idx = workspace_index_of(ws);

        gtk_stack_remove(GTK_STACK(grandparent), GTK_WIDGET(parent_paned));

        char stack_name[32];
        snprintf(stack_name, sizeof(stack_name), "ws-%d",
                 ws_idx >= 0 ? ws_idx : 0);
        gtk_stack_add_named(GTK_STACK(grandparent), sibling, stack_name);
        gtk_stack_set_visible_child(GTK_STACK(grandparent), sibling);

        ws->container = sibling;

        if (GTK_IS_NOTEBOOK(sibling))
            ws->notebook = sibling;
    }

    g_object_unref(sibling);
}

/* (activity helpers are defined earlier in this file) */

/* ── Notebook switch-page: clear activity on focused terminal ──── */

static void
on_notebook_switch_page(GtkNotebook *nb, GtkWidget *page,
                        guint page_num, gpointer user_data)
{
    (void)nb;
    (void)page_num;
    Workspace *ws = user_data;

    if (GHOSTTY_IS_TERMINAL(page)) {
        ghostty_terminal_clear_activity(GHOSTTY_TERMINAL(page));
        /* Refresh tab labels + sidebar to remove the dot */
        workspace_refresh_tab_labels(ws);
        workspace_refresh_sidebar_label(ws);
    }
}

/* ── Right-click context menu on sidebar rows ──────────────────── */

static void
on_sidebar_ctx_rename_activate(GSimpleAction *action, GVariant *param,
                               gpointer user_data)
{
    (void)action;
    (void)param;
    SidebarCtxData *ctx = user_data;
    Workspace *ws = ctx->workspace;
    if (!ws || !ws->sidebar_label)
        return;

    /* Trigger inline rename on the sidebar label's parent box */
    GtkWidget *box = gtk_widget_get_parent(ws->sidebar_label);
    if (!box) return;

    RenameData *rd = g_object_get_data(G_OBJECT(box), "rename-data");
    if (!rd) return;

    /* Same logic as on_label_double_click with n_press == 2 */
    GtkWidget *parent_box = rd->event_box;
    const char *current_text = gtk_label_get_text(GTK_LABEL(rd->label));

    gtk_box_remove(GTK_BOX(parent_box), rd->label);

    GtkWidget *entry = gtk_entry_new();
    GtkEntryBuffer *buf = gtk_entry_get_buffer(GTK_ENTRY(entry));
    gtk_entry_buffer_set_text(buf, current_text, -1);
    gtk_widget_set_hexpand(entry, FALSE);
    gtk_widget_set_size_request(entry, 80, -1);

    g_signal_connect(entry, "activate",
                     G_CALLBACK(on_rename_entry_activate), rd);

    GtkEventController *focus_ctrl = gtk_event_controller_focus_new();
    g_signal_connect(focus_ctrl, "leave",
                     G_CALLBACK(on_rename_entry_focus_leave), rd);
    gtk_widget_add_controller(entry, focus_ctrl);

    gtk_box_append(GTK_BOX(parent_box), entry);
    gtk_widget_set_visible(entry, TRUE);
    gtk_widget_grab_focus(entry);
}

static void
on_sidebar_ctx_delete_activate(GSimpleAction *action, GVariant *param,
                               gpointer user_data)
{
    (void)action;
    (void)param;
    SidebarCtxData *ctx = user_data;
    if (g_terminal_stack && g_workspace_list)
        workspace_remove(ctx->ws_idx, g_terminal_stack, g_workspace_list);
}

static void
on_sidebar_right_click(GtkGestureClick *gesture, int n_press,
                       double x, double y, gpointer user_data)
{
    (void)n_press;
    SidebarCtxData *ctx = user_data;
    GtkWidget *widget = gtk_event_controller_get_widget(
        GTK_EVENT_CONTROLLER(gesture));

    GMenu *menu = g_menu_new();
    char action_rename[64];
    char action_delete[64];
    snprintf(action_rename, sizeof(action_rename), "sidebar-ctx-%d.rename", ctx->ws_idx);
    snprintf(action_delete, sizeof(action_delete), "sidebar-ctx-%d.delete", ctx->ws_idx);

    g_menu_append(menu, "Rename", action_rename);
    g_menu_append(menu, "Delete", action_delete);

    /* Create action group */
    char group_name[64];
    snprintf(group_name, sizeof(group_name), "sidebar-ctx-%d", ctx->ws_idx);

    GSimpleActionGroup *ag = g_simple_action_group_new();

    GSimpleAction *act_rename = g_simple_action_new("rename", NULL);
    g_signal_connect(act_rename, "activate",
                     G_CALLBACK(on_sidebar_ctx_rename_activate), ctx);
    g_action_map_add_action(G_ACTION_MAP(ag), G_ACTION(act_rename));

    GSimpleAction *act_delete = g_simple_action_new("delete", NULL);
    g_signal_connect(act_delete, "activate",
                     G_CALLBACK(on_sidebar_ctx_delete_activate), ctx);
    g_action_map_add_action(G_ACTION_MAP(ag), G_ACTION(act_delete));

    gtk_widget_insert_action_group(widget, group_name, G_ACTION_GROUP(ag));

    GtkWidget *popover = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
    gtk_widget_set_parent(popover, widget);

    GdkRectangle rect = { (int)x, (int)y, 1, 1 };
    gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);
    gtk_popover_popup(GTK_POPOVER(popover));

    g_object_unref(menu);
    g_object_unref(act_rename);
    g_object_unref(act_delete);
    g_object_unref(ag);
}
