#include "workspace.h"
#include "app_settings.h"
#include "close_confirm.h"
#include "ghostty_terminal.h"
#include "hover_focus.h"
#include "port_scanner.h"
#include "project_icon_cache.h"
#include "resize_overlay.h"
#include "session.h"
#include "shortcuts.h"
#include <stdio.h>
#include <string.h>

GPtrArray *workspaces = NULL;
int current_workspace = 0;
static gboolean app_shutting_down = FALSE;
static guint64 next_pane_serial = 1;
static guint notification_flash_timer_id = 0;
static gboolean notification_flash_visible = FALSE;

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
static void workspace_add_terminal_to_notebook_cwd(Workspace *ws,
    GtkNotebook *notebook, ghostty_app_t app, const char *cwd);
static void setup_tab_label_dnd(GtkWidget *label, GtkWidget *terminal,
                                GtkNotebook *notebook, Workspace *ws);
static void on_notebook_switch_page(GtkNotebook *nb, GtkWidget *page,
                                    guint page_num, gpointer user_data);
typedef struct _RenameData RenameData;
static void finish_rename(GtkEntry *entry, RenameData *rd);
static void build_tab_label_text(GhosttyTerminal *term, const char *title,
                                 char *buf, size_t bufsz);
static void on_terminal_state_changed(GObject *obj, gpointer user_data);
static void focus_terminal_page(GtkWidget *page);
static void focus_terminal_page_later(GtkWidget *page);
static GtkWidget *page_linked_terminal(GtkWidget *page);
static GtkWidget *terminal_linked_dummy(GtkWidget *terminal);
static GtkWidget *notebook_terminal_at(GtkNotebook *notebook, int page_num);
static int notebook_page_for_terminal(GtkNotebook *notebook, GtkWidget *terminal);
static GtkNotebook *terminal_parent_notebook(GtkWidget *terminal);
static GtkWidget *create_terminal_tab(Workspace *ws, GtkNotebook *notebook,
                                      const char *cwd, int page_num);
static gboolean move_terminal_to_notebook(Workspace *src_ws, GtkNotebook *src_nb,
                                          GtkWidget *terminal, Workspace *dest_ws,
                                          GtkNotebook *dest_nb);
static void workspace_set_active_pane(Workspace *ws, GtkNotebook *notebook);
static int workspace_equalize_leaf_count(GtkWidget *widget,
                                         gboolean filter_orientation,
                                         GtkOrientation orientation);
static void workspace_equalize_widget(GtkWidget *widget,
                                      gboolean filter_orientation,
                                      GtkOrientation orientation);
static gboolean workspace_has_pane(Workspace *ws, GtkNotebook *notebook);
static void workspace_on_paned_position_notify(GObject *object,
                                               GParamSpec *pspec,
                                               gpointer user_data);
static gboolean workspace_drag_value_terminal(const GValue *value,
                                              GtkWidget **terminal_out,
                                              GtkNotebook **src_nb_out,
                                              Workspace **src_ws_out);
static void workspace_request_terminal_icon(GtkWidget *terminal,
                                            const char *path);
static void refresh_terminal_tab_icon(GtkWidget *terminal);
static void refresh_all_workspace_sidebar_labels(void);
static void workspace_focus_first_terminal(Workspace *ws);

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

static GtkWidget *
page_linked_terminal(GtkWidget *page)
{
    GtkWidget *terminal;

    if (!page)
        return NULL;
    if (GHOSTTY_IS_TERMINAL(page))
        return page;

    terminal = g_object_get_data(G_OBJECT(page), "linked-terminal");
    return (terminal && GHOSTTY_IS_TERMINAL(terminal)) ? terminal : NULL;
}

static void
workspace_focus_first_terminal(Workspace *ws)
{
    GtkNotebook *pane;
    GtkWidget *terminal;
    const char *cwd;

    if (!ws)
        return;

    pane = GTK_NOTEBOOK(ws->notebook);
    if (!GTK_IS_NOTEBOOK(pane) || gtk_notebook_get_n_pages(pane) <= 0)
        return;

    workspace_set_active_pane(ws, pane);
    gtk_notebook_set_current_page(pane, 0);

    terminal = notebook_terminal_at(pane, 0);
    if (!terminal || !GHOSTTY_IS_TERMINAL(terminal))
        return;

    cwd = ghostty_terminal_get_cwd(GHOSTTY_TERMINAL(terminal));
    if (cwd && cwd[0]) {
        snprintf(ws->cwd, sizeof(ws->cwd), "%s", cwd);
        workspace_detect_git(ws);
    }

    focus_terminal_page(terminal);
    focus_terminal_page_later(terminal);
    ghostty_terminal_clear_activity(GHOSTTY_TERMINAL(terminal));
    workspace_clear_tab_notification(pane, 0);
    workspace_refresh_tab_labels(ws);
    workspace_refresh_sidebar_label(ws);
}

static GtkWidget *
terminal_linked_dummy(GtkWidget *terminal)
{
    if (!terminal || !GHOSTTY_IS_TERMINAL(terminal))
        return NULL;

    return ghostty_terminal_get_dummy_target(GHOSTTY_TERMINAL(terminal));
}

static GtkWidget *
notebook_terminal_at(GtkNotebook *notebook, int page_num)
{
    return page_linked_terminal(gtk_notebook_get_nth_page(notebook, page_num));
}

static int
notebook_page_for_terminal(GtkNotebook *notebook, GtkWidget *terminal)
{
    GtkWidget *dummy = terminal_linked_dummy(terminal);

    if (dummy) {
        int page_num = gtk_notebook_page_num(notebook, dummy);
        if (page_num >= 0)
            return page_num;
    }

    for (int i = 0; i < gtk_notebook_get_n_pages(notebook); i++) {
        if (notebook_terminal_at(notebook, i) == terminal)
            return i;
    }

    return -1;
}

static GtkNotebook *
terminal_parent_notebook(GtkWidget *terminal)
{
    GtkWidget *dummy = terminal_linked_dummy(terminal);
    GtkWidget *notebook = dummy ? gtk_widget_get_ancestor(dummy,
                                                          GTK_TYPE_NOTEBOOK)
                                : NULL;
    return GTK_IS_NOTEBOOK(notebook) ? GTK_NOTEBOOK(notebook) : NULL;
}

static void
terminal_set_project_icon(GtkWidget *terminal, const char *root, const char *icon_path)
{
    if (!terminal || !GHOSTTY_IS_TERMINAL(terminal))
        return;

    g_object_set_data_full(G_OBJECT(terminal), "project-icon-root",
                           g_strdup(root ? root : ""), g_free);
    g_object_set_data_full(G_OBJECT(terminal), "project-icon-path",
                           g_strdup(icon_path ? icon_path : ""), g_free);
}

static const char *
terminal_project_icon(GtkWidget *terminal)
{
    const char *icon_path;

    if (!terminal || !GHOSTTY_IS_TERMINAL(terminal))
        return NULL;

    icon_path = g_object_get_data(G_OBJECT(terminal), "project-icon-path");
    if (!icon_path || !icon_path[0])
        return NULL;

    if (!g_file_test(icon_path, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_REGULAR)) {
        terminal_set_project_icon(terminal,
                                  g_object_get_data(G_OBJECT(terminal),
                                                    "project-icon-root"),
                                  NULL);
        return NULL;
    }

    return icon_path;
}

static GtkWidget *
row_icon_widget(GtkWidget *row_box)
{
    if (!row_box)
        return NULL;
    return g_object_get_data(G_OBJECT(row_box), "row-icon-widget");
}

static const char *
emoji_for_path(const char *path)
{
    const char *home = g_get_home_dir();

    if (!path || !path[0])
        return "📁";
    if (home && strcmp(path, home) == 0)
        return "🏠";
    if (strcmp(path, "/") == 0)
        return "🖥️";
    if (strcmp(path, "/tmp") == 0 || strcmp(path, "/var/tmp") == 0)
        return "🧪";
    if (strcmp(path, "/etc") == 0)
        return "⚙️";
    if (strcmp(path, "/usr") == 0)
        return "📦";
    if (strcmp(path, "/var") == 0)
        return "🗄️";
    if (strcmp(path, "/opt") == 0)
        return "🧰";
    if (strcmp(path, "/dev") == 0)
        return "🔌";
    if (strcmp(path, "/mnt") == 0 || strcmp(path, "/media") == 0)
        return "💽";
    if (strcmp(path, "/srv") == 0)
        return "🚀";
    if (strcmp(path, "/home") == 0)
        return "🏘️";

    return "📁";
}

static void
set_row_icon(GtkWidget *row_box, const char *icon_path,
             const char *emoji, int pixel_size)
{
    GtkWidget *stack = row_icon_widget(row_box);
    GtkWidget *image;
    GtkWidget *image_box;
    GtkWidget *label;

    if (!GTK_IS_STACK(stack))
        return;

    image = g_object_get_data(G_OBJECT(stack), "row-icon-image");
    image_box = g_object_get_data(G_OBJECT(stack), "row-icon-image-box");
    label = g_object_get_data(G_OBJECT(stack), "row-icon-label");
    if (!GTK_IS_IMAGE(image) || !GTK_IS_WIDGET(image_box) || !GTK_IS_LABEL(label))
        return;

    gtk_widget_set_size_request(stack, pixel_size, pixel_size);
    gtk_widget_set_size_request(image_box, pixel_size, pixel_size);
    gtk_image_set_pixel_size(GTK_IMAGE(image), pixel_size);
    gtk_widget_set_size_request(image, pixel_size, pixel_size);
    gtk_widget_set_size_request(label, pixel_size, pixel_size);
    if (icon_path && icon_path[0] &&
        g_file_test(icon_path, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_REGULAR)) {
        gtk_image_set_from_file(GTK_IMAGE(image), icon_path);
        gtk_stack_set_visible_child(GTK_STACK(stack), image_box);
        gtk_widget_set_visible(stack, TRUE);
    } else {
        gtk_image_clear(GTK_IMAGE(image));
        gtk_label_set_text(GTK_LABEL(label), emoji ? emoji : "📁");
        gtk_stack_set_visible_child(GTK_STACK(stack), label);
        gtk_widget_set_visible(stack, TRUE);
    }
}

static GtkWidget *
workspace_first_terminal(Workspace *ws)
{
    GtkNotebook *nb;

    if (!ws || !ws->pane_notebooks || ws->pane_notebooks->len == 0)
        return NULL;

    nb = g_ptr_array_index(ws->pane_notebooks, 0);
    if (!GTK_IS_NOTEBOOK(nb) || gtk_notebook_get_n_pages(nb) <= 0)
        return NULL;

    return notebook_terminal_at(nb, 0);
}

static const char *
workspace_sidebar_icon_path(Workspace *ws)
{
    GtkWidget *terminal;
    const char *cwd;
    const char *cached;

    terminal = workspace_first_terminal(ws);
    if (!terminal)
        return NULL;

    cached = terminal_project_icon(terminal);
    if (cached)
        return cached;

    cwd = ghostty_terminal_get_cwd(GHOSTTY_TERMINAL(terminal));
    if (!cwd || !cwd[0])
        return NULL;

    cached = project_icon_cache_lookup_for_path(cwd);
    if (cached) {
        char *root = project_icon_cache_root_for_path(cwd);
        terminal_set_project_icon(terminal, root, cached);
        g_free(root);
    }

    return cached;
}

static const char *
workspace_sidebar_emoji(Workspace *ws)
{
    GtkWidget *terminal = workspace_first_terminal(ws);
    const char *cwd = NULL;

    if (terminal && GHOSTTY_IS_TERMINAL(terminal))
        cwd = ghostty_terminal_get_cwd(GHOSTTY_TERMINAL(terminal));
    if ((!cwd || !cwd[0]) && ws)
        cwd = ws->cwd;

    return emoji_for_path(cwd);
}

static void
refresh_terminal_tab_icon(GtkWidget *terminal)
{
    GtkNotebook *nb;
    GtkWidget *dummy;
    GtkWidget *tab_widget;
    int page_num;

    if (!terminal || !GHOSTTY_IS_TERMINAL(terminal))
        return;

    nb = terminal_parent_notebook(terminal);
    if (!nb)
        return;

    page_num = notebook_page_for_terminal(nb, terminal);
    if (page_num < 0)
        return;

    dummy = gtk_notebook_get_nth_page(nb, page_num);
    tab_widget = dummy ? gtk_notebook_get_tab_label(nb, dummy) : NULL;
    set_row_icon(tab_widget, terminal_project_icon(terminal),
                 emoji_for_path(ghostty_terminal_get_cwd(
                     GHOSTTY_TERMINAL(terminal))), 24);
}

static void
refresh_all_workspace_sidebar_labels(void)
{
    if (!workspaces)
        return;

    for (guint i = 0; i < workspaces->len; i++) {
        Workspace *ws = g_ptr_array_index(workspaces, i);
        workspace_refresh_sidebar_label(ws);
    }
}

typedef struct {
    GtkWidget *terminal;
} PendingTerminalIconUpdate;

static void
pending_terminal_icon_update_free(gpointer data)
{
    PendingTerminalIconUpdate *pending_update = data;

    if (!pending_update)
        return;
    if (pending_update->terminal) {
        g_object_remove_weak_pointer(G_OBJECT(pending_update->terminal),
                                     (gpointer *)&pending_update->terminal);
    }
    g_free(pending_update);
}

static void
on_project_icon_resolved(const char *root, const char *icon_path, gpointer user_data)
{
    PendingTerminalIconUpdate *pending_update = user_data;
    const char *current_root;

    if (!pending_update || !pending_update->terminal ||
        !GHOSTTY_IS_TERMINAL(pending_update->terminal))
        return;

    current_root = g_object_get_data(G_OBJECT(pending_update->terminal),
                                     "project-icon-root");
    if (g_strcmp0(current_root ? current_root : "", root ? root : "") != 0)
        return;

    terminal_set_project_icon(pending_update->terminal, root, icon_path);
    refresh_terminal_tab_icon(pending_update->terminal);
    refresh_all_workspace_sidebar_labels();
    session_queue_save();
}

static void
workspace_request_terminal_icon(GtkWidget *terminal, const char *path)
{
    char *root;
    const char *cached;
    PendingTerminalIconUpdate *pending_update;

    if (!terminal || !GHOSTTY_IS_TERMINAL(terminal))
        return;

    root = project_icon_cache_root_for_path(path);
    if (!root) {
        terminal_set_project_icon(terminal, NULL, NULL);
        refresh_terminal_tab_icon(terminal);
        refresh_all_workspace_sidebar_labels();
        return;
    }

    cached = project_icon_cache_lookup(root);
    terminal_set_project_icon(terminal, root, cached);
    refresh_terminal_tab_icon(terminal);
    refresh_all_workspace_sidebar_labels();

    if (cached) {
        g_free(root);
        return;
    }

    pending_update = g_new0(PendingTerminalIconUpdate, 1);
    pending_update->terminal = terminal;
    g_object_add_weak_pointer(G_OBJECT(terminal),
                              (gpointer *)&pending_update->terminal);
    project_icon_cache_request(path, on_project_icon_resolved,
                               pending_update,
                               pending_terminal_icon_update_free);
    g_free(root);
}

static void
workspace_set_active_pane(Workspace *ws, GtkNotebook *notebook)
{
    if (!ws || !GTK_IS_NOTEBOOK(notebook) || !ws->pane_notebooks)
        return;

    for (guint i = 0; i < ws->pane_notebooks->len; i++) {
        if (g_ptr_array_index(ws->pane_notebooks, i) == notebook) {
            ws->active_pane = notebook;
            return;
        }
    }
}

static gboolean
workspace_has_pane(Workspace *ws, GtkNotebook *notebook)
{
    if (!ws || !ws->pane_notebooks || !GTK_IS_NOTEBOOK(notebook))
        return FALSE;

    for (guint i = 0; i < ws->pane_notebooks->len; i++) {
        if (g_ptr_array_index(ws->pane_notebooks, i) == notebook)
            return TRUE;
    }

    return FALSE;
}

static gboolean
tab_has_notification_flash(GtkWidget *terminal)
{
    return terminal &&
           GPOINTER_TO_INT(g_object_get_data(G_OBJECT(terminal),
                                             "notification-flash")) != 0;
}

static void
set_tab_flash_class(GtkNotebook *nb, int page_num, gboolean visible)
{
    GtkWidget *page;
    GtkWidget *tab_widget;

    if (!GTK_IS_NOTEBOOK(nb))
        return;

    page = gtk_notebook_get_nth_page(nb, page_num);
    if (!page)
        return;

    tab_widget = gtk_notebook_get_tab_label(nb, page);
    if (!tab_widget)
        return;

    if (visible)
        gtk_widget_add_css_class(tab_widget, "tab-blink");
    else
        gtk_widget_remove_css_class(tab_widget, "tab-blink");
}

static gboolean
workspace_has_notification_flash(void)
{
    if (!workspaces)
        return FALSE;

    for (guint wi = 0; wi < workspaces->len; wi++) {
        Workspace *ws = g_ptr_array_index(workspaces, wi);
        if (!ws || !ws->pane_notebooks)
            continue;

        for (guint pi = 0; pi < ws->pane_notebooks->len; pi++) {
            GtkNotebook *nb = g_ptr_array_index(ws->pane_notebooks, pi);
            if (!GTK_IS_NOTEBOOK(nb))
                continue;

            for (int ti = 0; ti < gtk_notebook_get_n_pages(nb); ti++) {
                GtkWidget *terminal = notebook_terminal_at(nb, ti);
                if (tab_has_notification_flash(terminal))
                    return TRUE;
            }
        }
    }

    return FALSE;
}

static gboolean
notification_flash_tick_cb(gpointer user_data)
{
    (void)user_data;

    if (!workspace_has_notification_flash()) {
        notification_flash_timer_id = 0;
        notification_flash_visible = FALSE;
        return G_SOURCE_REMOVE;
    }

    notification_flash_visible = !notification_flash_visible;

    for (guint wi = 0; workspaces && wi < workspaces->len; wi++) {
        Workspace *ws = g_ptr_array_index(workspaces, wi);
        if (!ws || !ws->pane_notebooks)
            continue;

        for (guint pi = 0; pi < ws->pane_notebooks->len; pi++) {
            GtkNotebook *nb = g_ptr_array_index(ws->pane_notebooks, pi);
            if (!GTK_IS_NOTEBOOK(nb))
                continue;

            for (int ti = 0; ti < gtk_notebook_get_n_pages(nb); ti++) {
                GtkWidget *terminal = notebook_terminal_at(nb, ti);
                if (!terminal)
                    continue;

                set_tab_flash_class(nb, ti,
                                    tab_has_notification_flash(terminal) &&
                                    notification_flash_visible);
            }
        }
    }

    return G_SOURCE_CONTINUE;
}

static void
ensure_notification_flash_timer(void)
{
    if (notification_flash_timer_id != 0 || !workspace_has_notification_flash())
        return;

    notification_flash_visible = TRUE;
    notification_flash_timer_id =
        g_timeout_add(800, notification_flash_tick_cb, NULL);
}

static void
workspace_on_paned_position_notify(GObject *object,
                                   GParamSpec *pspec,
                                   gpointer user_data)
{
    (void)object;
    (void)pspec;
    (void)user_data;
    session_queue_save();
}

const char *
workspace_get_pane_id(GtkNotebook *pane)
{
    if (!GTK_IS_NOTEBOOK(pane))
        return NULL;
    return g_object_get_data(G_OBJECT(pane), "pane-id");
}

int
workspace_get_pane_index(Workspace *ws, GtkNotebook *pane)
{
    if (!ws || !GTK_IS_NOTEBOOK(pane) || !ws->pane_notebooks)
        return -1;

    for (guint i = 0; i < ws->pane_notebooks->len; i++) {
        if (g_ptr_array_index(ws->pane_notebooks, i) == pane)
            return (int)i;
    }

    return -1;
}

GtkNotebook *
workspace_get_pane_by_index(Workspace *ws, int pane_idx)
{
    if (!ws || !ws->pane_notebooks || pane_idx < 0 ||
        pane_idx >= (int)ws->pane_notebooks->len)
        return NULL;

    return g_ptr_array_index(ws->pane_notebooks, pane_idx);
}

GtkNotebook *
workspace_get_pane_by_id(Workspace *ws, const char *pane_id)
{
    if (!ws || !ws->pane_notebooks || !pane_id || !pane_id[0])
        return NULL;

    for (guint i = 0; i < ws->pane_notebooks->len; i++) {
        GtkNotebook *pane = g_ptr_array_index(ws->pane_notebooks, i);
        const char *candidate = workspace_get_pane_id(pane);
        if (candidate && strcmp(candidate, pane_id) == 0)
            return pane;
    }

    return NULL;
}

gboolean
workspace_focus_pane(Workspace *ws, GtkNotebook *pane)
{
    if (!ws || !GTK_IS_NOTEBOOK(pane))
        return FALSE;

    if (workspace_get_pane_index(ws, pane) < 0)
        return FALSE;

    workspace_set_active_pane(ws, pane);

    int page = gtk_notebook_get_current_page(pane);
    if (page < 0 || page >= gtk_notebook_get_n_pages(pane))
        return FALSE;

    GtkWidget *terminal = notebook_terminal_at(pane, page);
    if (!terminal)
        return FALSE;

    focus_terminal_page(terminal);
    focus_terminal_page_later(terminal);
    return TRUE;
}

/* ── Activity detection ────────────────────────────────────────── */

gboolean
workspace_has_activity(Workspace *ws)
{
    if (!ws || !ws->terminals)
        return FALSE;
    guint i;
    for (i = 0; i < ws->terminals->len; i++) {
        GtkWidget *w = g_ptr_array_index(ws->terminals, i);
        if (!GHOSTTY_IS_TERMINAL(w)) continue;
        if (ghostty_terminal_has_activity(GHOSTTY_TERMINAL(w)))
            return TRUE;
    }
    return FALSE;
}

void
workspace_mark_tab_notification(GtkNotebook *pane, int page_num)
{
    GtkWidget *terminal;
    Workspace *ws;

    if (!GTK_IS_NOTEBOOK(pane) || page_num < 0 ||
        page_num >= gtk_notebook_get_n_pages(pane))
        return;

    ws = g_object_get_data(G_OBJECT(pane), "workspace-ptr");
    if (ws && workspace_index_of(ws) == current_workspace &&
        gtk_notebook_get_current_page(pane) == page_num)
        return;

    terminal = notebook_terminal_at(pane, page_num);
    if (!terminal)
        return;

    g_object_set_data(G_OBJECT(terminal), "notification-flash",
                      GINT_TO_POINTER(1));
    set_tab_flash_class(pane, page_num, TRUE);
    ensure_notification_flash_timer();
}

void
workspace_clear_tab_notification(GtkNotebook *pane, int page_num)
{
    GtkWidget *terminal;

    if (!GTK_IS_NOTEBOOK(pane) || page_num < 0 ||
        page_num >= gtk_notebook_get_n_pages(pane))
        return;

    terminal = notebook_terminal_at(pane, page_num);
    if (!terminal)
        return;

    g_object_set_data(G_OBJECT(terminal), "notification-flash", NULL);
    set_tab_flash_class(pane, page_num, FALSE);
}

/* ── Tab label refresh ─────────────────────────────────────────── */

void
workspace_refresh_tab_labels(Workspace *ws)
{
    if (!ws || !ws->pane_notebooks)
        return;
    guint pi;
    for (pi = 0; pi < ws->pane_notebooks->len; pi++) {
        GtkNotebook *nb = g_ptr_array_index(ws->pane_notebooks, pi);
        if (!GTK_IS_NOTEBOOK(nb)) continue;
        int n_pages = gtk_notebook_get_n_pages(nb);
        int i;
        for (i = 0; i < n_pages; i++) {
            GtkWidget *page = gtk_notebook_get_nth_page(nb, i);
            GtkWidget *terminal = page_linked_terminal(page);
            if (!terminal || !GHOSTTY_IS_TERMINAL(terminal))
                continue;
            GhosttyTerminal *term = GHOSTTY_TERMINAL(terminal);
            GtkWidget *tab_widget = gtk_notebook_get_tab_label(nb, page);
            if (!tab_widget)
                continue;
            if (g_object_get_data(G_OBJECT(tab_widget), "rename-in-progress"))
                continue;
            /* Find GtkLabel in the tab widget box */
            GtkWidget *inner = NULL;
            for (GtkWidget *w = gtk_widget_get_first_child(tab_widget);
                 w; w = gtk_widget_get_next_sibling(w)) {
                if (GTK_IS_LABEL(w)) { inner = w; break; }
            }
            if (!inner)
                continue;
            if (!gtk_widget_get_parent(inner))
                continue;
            if (g_object_get_data(G_OBJECT(inner), "user-renamed"))
                continue;
            const char *title = ghostty_terminal_get_title(term);
            char buf[128];
            build_tab_label_text(term, title, buf, sizeof(buf));
            gtk_label_set_text(GTK_LABEL(inner), buf);
            gtk_widget_set_tooltip_text(inner,
                                        (title && title[0]) ? title : "Terminal");
        }
    }
}

/* ── Sidebar label refresh ──────────────────────────────────────── */

/* Shorten a path for display: "~/foo/.../last" or just "last" if tight */
static void shorten_cwd_for_sidebar(const char *cwd, char *buf, size_t bufsz) {
    if (!cwd || !cwd[0]) { buf[0] = '\0'; return; }

    const char *home = g_get_home_dir();
    size_t homelen = home ? strlen(home) : 0;
    const char *display = cwd;
    char tilde_path[256];

    /* Replace $HOME with ~ */
    if (home && homelen > 0 && strncmp(cwd, home, homelen) == 0) {
        snprintf(tilde_path, sizeof(tilde_path), "~%s", cwd + homelen);
        display = tilde_path;
    }

    /* Find last component */
    const char *last_slash = strrchr(display, '/');
    const char *last = last_slash ? last_slash + 1 : display;
    if (!*last && last_slash > display) {
        /* Trailing slash — back up one more */
        const char *p = last_slash - 1;
        while (p > display && *p != '/') p--;
        if (*p == '/') p++;
        last = p;
    }

    /* If path is short enough, show it all */
    if (strlen(display) <= bufsz - 1) {
        snprintf(buf, bufsz, "%s", display);
    } else {
        /* Show "~/.../last" */
        snprintf(buf, bufsz, "~/.../%.20s", last);
    }
}

void workspace_refresh_sidebar_label(Workspace *ws) {
    GtkWidget *row_box;

    if (!ws || !ws->sidebar_label) return;
    if (!GTK_IS_LABEL(ws->sidebar_label) ||
        g_object_get_data(G_OBJECT(ws->sidebar_label), "rename-in-progress") ||
        !gtk_widget_get_parent(ws->sidebar_label))
        return;
    row_box = gtk_widget_get_parent(ws->sidebar_label);

    gboolean has_act = workspace_has_activity(ws);
    char buf[256];

    /* Activity dot + workspace name */
    const char *dot = has_act ? "\342\227\217 " : "";

    /* Git branch suffix */
    char branch[64] = "";
    if (ws->git_branch[0])
        snprintf(branch, sizeof(branch), " [%s]", ws->git_branch);

    /* Compact CWD */
    char short_cwd[32];
    shorten_cwd_for_sidebar(ws->cwd, short_cwd, sizeof(short_cwd));

    /* Build label: "● Name [branch]\n~/.../dir" */
    if (short_cwd[0]) {
        char *esc_line1 = g_markup_escape_text(
            g_strdup_printf("%s%s%s", dot, ws->name, branch), -1);
        char *esc_cwd = g_markup_escape_text(short_cwd, -1);
        snprintf(buf, sizeof(buf), "%s\n<small>%s</small>", esc_line1, esc_cwd);
        g_free(esc_line1);
        g_free(esc_cwd);
        gtk_label_set_markup(GTK_LABEL(ws->sidebar_label), buf);
    } else {
        snprintf(buf, sizeof(buf), "%s%s%s", dot, ws->name, branch);
        gtk_label_set_text(GTK_LABEL(ws->sidebar_label), buf);
    }

    /* Tooltip: full CWD path */
    if (ws->cwd[0])
        gtk_widget_set_tooltip_text(ws->sidebar_label, ws->cwd);
    else
        gtk_widget_set_tooltip_text(ws->sidebar_label, NULL);

    set_row_icon(row_box, workspace_sidebar_icon_path(ws),
                 workspace_sidebar_emoji(ws), 16);

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

    /* Update status bar on all terminals in this workspace */
    if (ws->terminals) {
        guint i;
        for (i = 0; i < ws->terminals->len; i++) {
            GhosttyTerminal *term = g_ptr_array_index(ws->terminals, i);
            const char *cwd = ghostty_terminal_get_cwd(term);
            ghostty_terminal_set_status(term, cwd, ws->git_branch);
        }
    }
}

void workspace_detect_git(Workspace *ws) {
    if (!ws) return;
    if (!ws->cwd[0]) {
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

/*
 * shorten_path: extract last directory component with "..." prefix.
 * e.g. "/home/user/projects/myapp" -> ".../myapp"
 * Writes into buf of size bufsz.  Returns buf.
 */
static const char *
shorten_path(const char *path, char *buf, size_t bufsz)
{
    const char *home = g_get_home_dir();

    if (!path || !path[0]) {
        snprintf(buf, bufsz, "Terminal");
        return buf;
    }
    if (home && strcmp(path, home) == 0) {
        snprintf(buf, bufsz, "~");
        return buf;
    }
    if (strcmp(path, "/") == 0) {
        snprintf(buf, bufsz, "/");
        return buf;
    }

    if (path[0] == '/') {
        const char *second = strchr(path + 1, '/');
        if (!second || second[1] == '\0') {
            snprintf(buf, bufsz, "%s", path);
            return buf;
        }
    }

    const char *last_slash = strrchr(path, '/');
    if (last_slash && last_slash[1]) {
        snprintf(buf, bufsz, ".../%s", last_slash + 1);
    } else {
        snprintf(buf, bufsz, "%.28s", path);
    }
    return buf;
}

static void
build_tab_label_text(GhosttyTerminal *term, const char *title, char *buf, size_t bufsz)
{
    /* Shorten the title (typically CWD-based) to last directory component */
    char short_title[64];
    shorten_path(title, short_title, sizeof(short_title));

    /* Activity indicator (green dot prefix) */
    const char *activity_prefix = "";
    if (ghostty_terminal_has_activity(term))
        activity_prefix = "\342\227\217 ";   /* "● " in UTF-8 */

    /* Progress bar suffix */
    char progress_suffix[48];
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

static void
on_terminal_state_changed(GObject *obj, gpointer user_data)
{
    (void)obj;
    (void)user_data;
    session_queue_save();
}

static void
on_terminal_pwd_changed(GhosttyTerminal *term, const char *cwd, gpointer user_data)
{
    Workspace *ws = user_data;
    GtkNotebook *pane;
    int current_page;
    GtkWidget *visible_terminal;

    workspace_request_terminal_icon(GTK_WIDGET(term), cwd);

    if (!ws || !cwd || !cwd[0])
        return;

    pane = terminal_parent_notebook(GTK_WIDGET(term));
    if (!pane || workspace_get_focused_pane(ws) != pane)
        return;

    current_page = gtk_notebook_get_current_page(pane);
    visible_terminal = notebook_terminal_at(pane, current_page);
    if (visible_terminal != GTK_WIDGET(term))
        return;

    snprintf(ws->cwd, sizeof(ws->cwd), "%s", cwd);
    workspace_detect_git(ws);
}

static void
focus_terminal_page(GtkWidget *page)
{
    if (!page || !GHOSTTY_IS_TERMINAL(page))
        return;

    ghostty_terminal_focus(GHOSTTY_TERMINAL(page));
    ghostty_terminal_queue_render(GHOSTTY_TERMINAL(page));
}

static gboolean
focus_terminal_page_idle_cb(gpointer user_data)
{
    GtkWidget *page = GTK_WIDGET(user_data);
    focus_terminal_page(page);
    g_object_unref(page);
    return G_SOURCE_REMOVE;
}

static void
focus_terminal_page_later(GtkWidget *page)
{
    if (!page || !GHOSTTY_IS_TERMINAL(page))
        return;

    g_object_ref(page);
    g_timeout_add(50, focus_terminal_page_idle_cb, page);
}

static gboolean
move_terminal_to_notebook(Workspace *src_ws, GtkNotebook *src_nb,
                          GtkWidget *terminal, Workspace *dest_ws,
                          GtkNotebook *dest_nb)
{
    GtkWidget *dummy;
    GtkWidget *tab_widget;
    int dest_page;

    if (!src_ws || !dest_ws || !GTK_IS_NOTEBOOK(src_nb) ||
        !GTK_IS_NOTEBOOK(dest_nb) || !GHOSTTY_IS_TERMINAL(terminal))
        return FALSE;

    if (src_nb == dest_nb)
        return FALSE;

    int src_page = notebook_page_for_terminal(src_nb, terminal);
    if (src_page < 0)
        return FALSE;

    dummy = terminal_linked_dummy(terminal);
    if (!dummy)
        return FALSE;

    tab_widget = gtk_notebook_get_tab_label(src_nb, dummy);
    if (tab_widget)
        g_object_ref(tab_widget);
    g_object_ref(dummy);

    gtk_notebook_remove_page(src_nb, src_page);

    if (gtk_notebook_get_n_pages(src_nb) == 0 &&
        src_ws->pane_notebooks && src_ws->pane_notebooks->len > 1) {
        workspace_close_pane(src_ws, src_nb);
    }

    gtk_notebook_append_page(dest_nb, dummy, tab_widget);
    gtk_notebook_set_tab_reorderable(dest_nb, dummy, TRUE);
    gtk_notebook_set_tab_detachable(dest_nb, dummy, TRUE);
    ghostty_terminal_set_dummy_target(GHOSTTY_TERMINAL(terminal), dummy);
    dest_page = gtk_notebook_page_num(dest_nb, dummy);
    gtk_notebook_set_current_page(dest_nb, dest_page);
    focus_terminal_page(terminal);
    focus_terminal_page_later(terminal);
    refresh_terminal_tab_icon(terminal);
    workspace_refresh_sidebar_label(src_ws);
    workspace_refresh_sidebar_label(dest_ws);
    shortcut_log_event("gesture", "tab.drag.move", "Drag tab to pane");

    if (tab_widget)
        g_object_unref(tab_widget);
    g_object_unref(dummy);

    session_queue_save();
    return TRUE;
}

static void on_title_changed(GhosttyTerminal *term, const char *title, gpointer label_ptr) {
    /* Skip if rename is in progress or label was destroyed/unparented */
    if (!GTK_IS_LABEL(label_ptr)) return;
    if (g_object_get_data(G_OBJECT(label_ptr), "rename-in-progress")) return;
    if (!gtk_widget_get_parent(GTK_WIDGET(label_ptr))) return;
    /* Skip if user manually renamed this tab */
    if (g_object_get_data(G_OBJECT(label_ptr), "user-renamed")) return;

    char buf[128];
    build_tab_label_text(term, title, buf, sizeof(buf));
    gtk_label_set_text(GTK_LABEL(label_ptr), buf);

    /* Set tooltip with full path so user can hover to see it */
    gtk_widget_set_tooltip_text(GTK_WIDGET(label_ptr),
                                (title && title[0]) ? title : "Terminal");
}

/* ── Feature 4: Double-click to rename (tab labels + sidebar rows) ─ */

/*
 * When double-click is detected on a label, replace it with a GtkEntry
 * for inline editing.  On Enter or focus-out, restore the label.
 */

/* Data for the inline rename operation */
struct _RenameData {
    GtkWidget *event_box;        /* The parent box holding label or entry */
    GtkWidget *label;            /* The GtkLabel */
    GtkWidget *terminal;         /* Associated terminal (NULL for sidebar) */
    Workspace *workspace;        /* Associated workspace (for sidebar rows) */
    gboolean is_workspace_row;   /* TRUE if this is a workspace sidebar rename */
};

static void on_rename_entry_activate(GtkEntry *entry, gpointer user_data);
static void on_rename_entry_focus_leave(GtkEventControllerFocus *ctrl,
                                        gpointer user_data);

static gboolean
on_rename_key_pressed(GtkEventControllerKey *ctrl, guint keyval,
                      guint keycode, GdkModifierType state, gpointer user_data)
{
    (void)ctrl; (void)keycode; (void)state;
    if (keyval == GDK_KEY_Escape) {
        RenameData *rd = user_data;
        /* Cancel: restore original label without changing text */
        GtkWidget *entry_widget = gtk_event_controller_get_widget(
            GTK_EVENT_CONTROLLER(ctrl));
        if (GTK_IS_ENTRY(entry_widget)) {
            /* Set buffer to original text so finish_rename doesn't change it */
            GtkEntryBuffer *buf = gtk_entry_get_buffer(GTK_ENTRY(entry_widget));
            gtk_entry_buffer_set_text(buf,
                gtk_label_get_text(GTK_LABEL(rd->label)), -1);
            finish_rename(GTK_ENTRY(entry_widget), rd);
        }
        return TRUE;
    }
    return FALSE;
}

static void
start_rename(RenameData *rd)
{
    if (!rd || !GTK_IS_BOX(rd->event_box) || !GTK_IS_LABEL(rd->label))
        return;
    if (g_object_get_data(G_OBJECT(rd->event_box), "rename-entry"))
        return;

    GtkWidget *parent = rd->event_box;
    const char *current_text = gtk_label_get_text(GTK_LABEL(rd->label));

    g_object_ref(rd->label);
    g_object_set_data(G_OBJECT(parent), "rename-in-progress", GINT_TO_POINTER(1));
    g_object_set_data(G_OBJECT(parent), "rename-entry", NULL);
    g_object_set_data(G_OBJECT(rd->label), "rename-in-progress", GINT_TO_POINTER(1));
    gtk_box_remove(GTK_BOX(parent), rd->label);

    GtkWidget *entry = gtk_entry_new();
    GtkEntryBuffer *buf = gtk_entry_get_buffer(GTK_ENTRY(entry));
    gtk_entry_buffer_set_text(buf, current_text, -1);
    gtk_widget_set_hexpand(entry, FALSE);
    gtk_widget_set_size_request(entry, 80, -1);

    g_signal_connect(entry, "activate",
                     G_CALLBACK(on_rename_entry_activate), rd);

    /* Escape cancels the rename */
    GtkEventController *key_ctrl = gtk_event_controller_key_new();
    g_signal_connect(key_ctrl, "key-pressed",
                     G_CALLBACK(on_rename_key_pressed), rd);
    gtk_widget_add_controller(entry, key_ctrl);

    g_object_set_data(G_OBJECT(parent), "rename-entry", entry);
    gtk_box_append(GTK_BOX(parent), entry);
    gtk_widget_set_visible(entry, TRUE);
    gtk_widget_set_size_request(entry, 120, -1);
    gtk_widget_grab_focus(entry);
    gtk_widget_queue_draw(parent);
}

static void
finish_rename(GtkEntry *entry, RenameData *rd)
{
    /* Guard: finish_rename can be called twice (activate + focus-leave) */
    if (!rd || !GTK_IS_BOX(rd->event_box) || !GTK_IS_LABEL(rd->label))
        return;

    GtkEntryBuffer *buf = gtk_entry_get_buffer(GTK_ENTRY(entry));
    const char *new_text = gtk_entry_buffer_get_text(buf);
    const char *old_text = gtk_label_get_text(GTK_LABEL(rd->label));
    gboolean changed = FALSE;
    if (new_text && new_text[0]) {
        changed = g_strcmp0(new_text, old_text) != 0;
        gtk_label_set_text(GTK_LABEL(rd->label), new_text);
        /* Mark as user-renamed so ghostty title changes don't overwrite */
        if (!rd->is_workspace_row)
            g_object_set_data(G_OBJECT(rd->label), "user-renamed",
                              GINT_TO_POINTER(1));
        if (rd->is_workspace_row && rd->workspace) {
            snprintf(rd->workspace->name, sizeof(rd->workspace->name),
                     "%.60s", new_text);
        }
    }

    /* Remove the entry, re-add the label */
    GtkWidget *entry_widget = GTK_WIDGET(entry);
    GtkWidget *parent = rd->event_box;
    if (g_object_get_data(G_OBJECT(parent), "rename-entry") != entry_widget)
        return;

    gtk_box_remove(GTK_BOX(parent), entry_widget);
    gtk_box_append(GTK_BOX(parent), rd->label);
    gtk_widget_set_visible(rd->label, TRUE);
    g_object_set_data(G_OBJECT(parent), "rename-entry", NULL);
    g_object_set_data(G_OBJECT(parent), "rename-in-progress", NULL);
    g_object_set_data(G_OBJECT(rd->label), "rename-in-progress", NULL);
    g_object_unref(rd->label); /* Balance the ref from start_rename */

    /* Now safe to refresh sidebar */
    if (rd->is_workspace_row && rd->workspace)
        workspace_refresh_sidebar_label(rd->workspace);

    if (changed)
        session_queue_save();
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
    start_rename(rd);
}

/*
 * Create a tab label widget with double-click-to-rename support.
 * Returns a GtkBox containing a GtkLabel with a gesture controller.
 * *out_label receives the inner GtkLabel pointer (for title-changed).
 */
/* Idle callback to refresh tab labels after a close */
static gboolean
tab_close_refresh_idle_cb(gpointer user_data)
{
    Workspace *ws = user_data;
    workspace_refresh_tab_labels(ws);
    return G_SOURCE_REMOVE;
}

gboolean
workspace_close_tab_at(Workspace *ws, GtkNotebook *nb, int page)
{
    GtkWidget *terminal;
    int n;

    if (!ws || !GTK_IS_NOTEBOOK(nb))
        return FALSE;
    if (page < 0 || page >= gtk_notebook_get_n_pages(nb))
        return FALSE;

    terminal = notebook_terminal_at(nb, page);
    if (!terminal || !GHOSTTY_IS_TERMINAL(terminal))
        return FALSE;

    n = gtk_notebook_get_n_pages(nb);
    if (n <= 1 && ws->pane_notebooks && ws->pane_notebooks->len <= 1)
        return FALSE;

    g_ptr_array_remove(ws->terminals, terminal);
    if (ws->overlay)
        gtk_overlay_remove_overlay(GTK_OVERLAY(ws->overlay), terminal);
    gtk_notebook_remove_page(nb, page);

    /* Defer tab label refresh to idle to avoid re-entrancy */
    g_idle_add(tab_close_refresh_idle_cb, ws);
    refresh_all_workspace_sidebar_labels();

    /* If notebook is now empty and there are other panes, close the pane */
    if (gtk_notebook_get_n_pages(nb) == 0 &&
        ws->pane_notebooks && ws->pane_notebooks->len > 1) {
        workspace_close_pane(ws, nb);
    }

    session_queue_save();
    return TRUE;
}

gboolean
workspace_close_terminal(Workspace *ws, GtkWidget *terminal)
{
    GtkNotebook *nb = NULL;
    int page = -1;

    if (!terminal || !ws || !GHOSTTY_IS_TERMINAL(terminal))
        return FALSE;

    nb = terminal_parent_notebook(terminal);
    if (nb)
        page = notebook_page_for_terminal(nb, terminal);

    if ((!nb || page < 0) && ws->pane_notebooks) {
        for (guint i = 0; i < ws->pane_notebooks->len; i++) {
            GtkNotebook *candidate = g_ptr_array_index(ws->pane_notebooks, i);
            int candidate_page;

            if (!GTK_IS_NOTEBOOK(candidate))
                continue;

            candidate_page = notebook_page_for_terminal(candidate, terminal);
            if (candidate_page >= 0) {
                nb = candidate;
                page = candidate_page;
                break;
            }
        }
    }

    if (!nb || page < 0)
        return FALSE;

    return workspace_close_tab_at(ws, nb, page);
}

gboolean
workspace_close_current_tab(Workspace *ws)
{
    GtkNotebook *nb;
    int page;

    if (!ws)
        return FALSE;

    nb = workspace_get_focused_pane(ws);
    if (!nb)
        return FALSE;

    page = gtk_notebook_get_current_page(nb);
    if (page < 0)
        return FALSE;

    return workspace_close_tab_at(ws, nb, page);
}

typedef struct {
    Workspace *ws;
    GtkWidget *terminal;
} PendingTabClose;

static void
pending_tab_close_free(gpointer data)
{
    PendingTabClose *pending = data;

    if (pending->terminal)
        g_object_unref(pending->terminal);
    g_free(pending);
}

static void
on_tab_close_confirmed(gboolean confirmed, gpointer user_data)
{
    PendingTabClose *pending = user_data;

    if (confirmed)
        workspace_close_terminal(pending->ws, pending->terminal);
}

/* Close button (X) on tab label */
static void
on_tab_close_clicked(GtkButton *btn, gpointer user_data)
{
    (void)user_data;
    GtkWidget *terminal = g_object_get_data(G_OBJECT(btn), "terminal-widget");
    Workspace *ws = g_object_get_data(G_OBJECT(btn), "workspace");
    GtkRoot *root;
    PendingTabClose *pending;

    if (!terminal || !ws || !GHOSTTY_IS_TERMINAL(terminal))
        return;

    root = gtk_widget_get_root(GTK_WIDGET(btn));
    if (!GTK_IS_WINDOW(root)) {
        workspace_close_terminal(ws, terminal);
        return;
    }

    pending = g_new0(PendingTabClose, 1);
    pending->ws = ws;
    pending->terminal = g_object_ref(terminal);
    close_confirm_request(GTK_WINDOW(root), CLOSE_CONFIRM_TAB,
                          on_tab_close_confirmed, pending,
                          pending_tab_close_free);
}

static GtkWidget *
create_editable_tab_label(const char *text, GtkWidget *terminal,
                          Workspace *ws, gboolean is_workspace_row,
                          GtkWidget **out_label)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *icon_stack = gtk_stack_new();
    GtkWidget *icon_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *icon = gtk_image_new();
    GtkWidget *emoji = gtk_label_new("📁");
    GtkWidget *label = gtk_label_new(text);
    gtk_widget_set_focusable(box, FALSE);
    gtk_widget_set_margin_end(icon_stack, 6);
    gtk_widget_set_valign(icon_stack, GTK_ALIGN_CENTER);
    gtk_widget_set_visible(icon_stack, TRUE);
    gtk_widget_set_size_request(icon_stack, 24, 24);
    gtk_widget_add_css_class(icon_box, "tab-art-box");
    gtk_widget_set_halign(icon_box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(icon_box, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(icon_box, 24, 24);
    gtk_box_append(GTK_BOX(icon_box), icon);
    gtk_widget_set_halign(emoji, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(emoji, GTK_ALIGN_CENTER);
    gtk_label_set_xalign(GTK_LABEL(emoji), 0.5f);
    gtk_label_set_yalign(GTK_LABEL(emoji), 0.5f);
    gtk_stack_add_named(GTK_STACK(icon_stack), icon_box, "image");
    gtk_stack_add_named(GTK_STACK(icon_stack), emoji, "emoji");
    gtk_stack_set_visible_child(GTK_STACK(icon_stack), emoji);
    gtk_box_append(GTK_BOX(box), icon_stack);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_box_append(GTK_BOX(box), label);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(box), spacer);
    g_object_set_data(G_OBJECT(box), "row-icon-widget", icon_stack);
    g_object_set_data(G_OBJECT(icon_stack), "row-icon-image", icon);
    g_object_set_data(G_OBJECT(icon_stack), "row-icon-image-box", icon_box);
    g_object_set_data(G_OBJECT(icon_stack), "row-icon-label", emoji);

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

    /* Add close button (X) for terminal tabs, not sidebar rows */
    if (!is_workspace_row && terminal) {
        GtkWidget *close_btn = gtk_button_new_from_icon_name("window-close-symbolic");
        gtk_button_set_has_frame(GTK_BUTTON(close_btn), FALSE);
        gtk_widget_set_focusable(close_btn, FALSE);
        gtk_widget_set_valign(close_btn, GTK_ALIGN_CENTER);
        gtk_widget_set_margin_start(close_btn, 4);
        g_object_set_data(G_OBJECT(close_btn), "terminal-widget", terminal);
        g_object_set_data(G_OBJECT(close_btn), "workspace", ws);
        g_signal_connect(close_btn, "clicked",
                         G_CALLBACK(on_tab_close_clicked), NULL);
        gtk_box_append(GTK_BOX(box), close_btn);
    }

    if (out_label)
        *out_label = label;
    return box;
}

static GtkWidget *
create_terminal_tab(Workspace *ws, GtkNotebook *notebook,
                    const char *cwd, int page_num)
{
    GtkWidget *dummy = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *terminal = ghostty_terminal_new((cwd && cwd[0]) ? cwd : NULL);
    GtkWidget *inner_label = NULL;
    GtkWidget *tab_label = create_editable_tab_label(
        "Terminal", terminal, ws, FALSE, &inner_label);

    gtk_widget_set_hexpand(dummy, TRUE);
    gtk_widget_set_vexpand(dummy, TRUE);
    g_object_set_data(G_OBJECT(dummy), "linked-terminal", terminal);
    g_object_set_data(G_OBJECT(terminal), "linked-dummy", dummy);

    g_ptr_array_add(ws->terminals, terminal);
    gtk_overlay_add_overlay(GTK_OVERLAY(ws->overlay), terminal);
    ghostty_terminal_set_dummy_target(GHOSTTY_TERMINAL(terminal), dummy);

    if (page_num >= 0)
        gtk_notebook_insert_page(notebook, dummy, tab_label, page_num);
    else
        gtk_notebook_append_page(notebook, dummy, tab_label);

    gtk_notebook_set_tab_reorderable(notebook, dummy, TRUE);
    gtk_notebook_set_tab_detachable(notebook, dummy, TRUE);

    g_signal_connect_object(terminal, "title-changed",
                            G_CALLBACK(on_title_changed), inner_label, 0);
    g_signal_connect(terminal, "title-changed",
                     G_CALLBACK(on_terminal_state_changed), NULL);
    g_signal_connect(terminal, "pwd-changed",
                     G_CALLBACK(on_terminal_state_changed), NULL);
    g_signal_connect(terminal, "pwd-changed",
                     G_CALLBACK(on_terminal_pwd_changed), NULL);

    gtk_widget_set_visible(dummy, TRUE);
    gtk_widget_set_visible(terminal, TRUE);
    workspace_request_terminal_icon(terminal, cwd);
    return terminal;
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
    GtkWidget *terminal = NULL;
    GtkNotebook *src_nb = NULL;
    Workspace *src_ws = NULL;

    if (!workspace_drag_value_terminal(value, &terminal, &src_nb, &src_ws))
        return FALSE;
    if (src_nb == dest_nb)
        return FALSE;

    /* Find destination workspace */
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

    return src_ws && dest_ws
        ? move_terminal_to_notebook(src_ws, src_nb, terminal, dest_ws, dest_nb)
        : FALSE;
}

/* ── Feature 1: DnD - Workspace sidebar drop target callbacks ───── */

static gboolean
on_ws_sidebar_drop(GtkDropTarget *target, const GValue *value,
                   double x, double y, gpointer user_data)
{
    (void)target; (void)x; (void)y;
    int dest_ws_idx = GPOINTER_TO_INT(user_data);
    GtkWidget *terminal = NULL;
    GtkNotebook *src_nb = NULL;
    Workspace *src_ws = NULL;

    if (dest_ws_idx < 0 || dest_ws_idx >= (int)workspaces->len)
        return FALSE;
    if (!workspace_drag_value_terminal(value, &terminal, &src_nb, &src_ws))
        return FALSE;

    Workspace *dest_ws = g_ptr_array_index(workspaces, dest_ws_idx);

    /* Don't drop on same workspace if it only has one notebook */
    if (src_ws == dest_ws && dest_ws->pane_notebooks->len == 1)
        return FALSE;

    GtkNotebook *dest_nb = GTK_NOTEBOOK(dest_ws->notebook);

    /* Switch to dest workspace */
    if (g_terminal_stack && g_workspace_list)
        workspace_switch(dest_ws_idx, g_terminal_stack, g_workspace_list);

    return move_terminal_to_notebook(src_ws, src_nb, terminal, dest_ws, dest_nb);
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
    GType drop_types[] = { GTK_TYPE_NOTEBOOK_PAGE, G_TYPE_BYTES };
    GtkDropTarget *drop = gtk_drop_target_new(GTK_TYPE_NOTEBOOK_PAGE,
                                              GDK_ACTION_MOVE);
    gtk_drop_target_set_gtypes(drop, drop_types, G_N_ELEMENTS(drop_types));
    g_signal_connect(drop, "drop",
                     G_CALLBACK(on_ws_sidebar_drop), GINT_TO_POINTER(ws_idx));
    gtk_widget_add_controller(row_widget, GTK_EVENT_CONTROLLER(drop));
}

static gboolean
workspace_drag_value_terminal(const GValue *value,
                              GtkWidget **terminal_out,
                              GtkNotebook **src_nb_out,
                              Workspace **src_ws_out)
{
    GtkWidget *terminal = NULL;
    GtkNotebook *src_nb = NULL;
    Workspace *src_ws = NULL;

    if (G_VALUE_HOLDS(value, G_TYPE_BYTES)) {
        GBytes *bytes = g_value_get_boxed(value);
        const TabDragData *dd;

        if (!bytes || g_bytes_get_size(bytes) != sizeof(TabDragData))
            return FALSE;

        dd = g_bytes_get_data(bytes, NULL);
        terminal = dd->terminal;
        src_nb = GTK_IS_NOTEBOOK(dd->source_notebook)
            ? GTK_NOTEBOOK(dd->source_notebook)
            : NULL;
        if (dd->source_ws_idx >= 0 && workspaces &&
            dd->source_ws_idx < (int)workspaces->len) {
            src_ws = g_ptr_array_index(workspaces, dd->source_ws_idx);
        }
    } else if (G_VALUE_HOLDS(value, GTK_TYPE_NOTEBOOK_PAGE)) {
        GtkNotebookPage *page = g_value_get_object(value);
        GtkWidget *child = page ? gtk_notebook_page_get_child(page) : NULL;
        GtkWidget *parent = child ? gtk_widget_get_parent(child) : NULL;

        terminal = page_linked_terminal(child);
        src_nb = GTK_IS_NOTEBOOK(parent) ? GTK_NOTEBOOK(parent) : NULL;
        if (src_nb)
            src_ws = g_object_get_data(G_OBJECT(src_nb), "workspace-ptr");
    } else {
        return FALSE;
    }

    if (!terminal || !src_nb || !src_ws || !GHOSTTY_IS_TERMINAL(terminal))
        return FALSE;

    if (terminal_out)
        *terminal_out = terminal;
    if (src_nb_out)
        *src_nb_out = src_nb;
    if (src_ws_out)
        *src_ws_out = src_ws;
    return TRUE;
}

/* ── Add terminal to notebook ───────────────────────────────────── */

static void
workspace_add_terminal_to_notebook(Workspace *ws, GtkNotebook *notebook,
                                   ghostty_app_t app)
{
    workspace_add_terminal_to_notebook_cwd(ws, notebook, app, NULL);
}

/* Add a terminal with a specific starting CWD */
static void
workspace_add_terminal_to_notebook_cwd(Workspace *ws, GtkNotebook *notebook,
                                       ghostty_app_t app, const char *cwd)
{
    GtkWidget *terminal;

    (void)app;
    terminal = create_terminal_tab(ws, notebook, cwd, -1);
    gtk_notebook_set_current_page(notebook,
        notebook_page_for_terminal(notebook, terminal));
}

void workspace_add_terminal(Workspace *ws, ghostty_app_t app) {
    /* Add to the first pane notebook (backwards compat). */
    workspace_add_terminal_to_notebook(ws, GTK_NOTEBOOK(ws->notebook), app);
}

void workspace_add_terminal_to_focused(Workspace *ws, ghostty_app_t app) {
    GtkNotebook *focused = workspace_get_focused_pane(ws);
    if (focused) {
        const char *cwd = NULL;
        int page = gtk_notebook_get_current_page(focused);
        GtkWidget *terminal = notebook_terminal_at(focused, page);

        if (GHOSTTY_IS_TERMINAL(terminal))
            cwd = ghostty_terminal_get_cwd(GHOSTTY_TERMINAL(terminal));

        workspace_add_terminal_to_notebook_cwd(
            ws, focused, app, (cwd && cwd[0]) ? cwd : NULL);
    } else {
        workspace_add_terminal(ws, app);
    }
}

void workspace_add_terminal_to_notebook_external(Workspace *ws,
                                                  GtkNotebook *notebook,
                                                  ghostty_app_t app) {
    workspace_add_terminal_to_notebook(ws, notebook, app);
}

void workspace_add_terminal_to_notebook_with_cwd(Workspace *ws,
                                                  GtkNotebook *notebook,
                                                  ghostty_app_t app,
                                                  const char *cwd) {
    workspace_add_terminal_to_notebook_cwd(ws, notebook, app, cwd);
}

void workspace_set_shutting_down(void) {
    app_shutting_down = TRUE;
}

gboolean
workspace_move_tab(int src_ws_idx, int src_pane_idx, int src_tab_idx,
                   int dest_ws_idx, int dest_pane_idx)
{
    if (!workspaces ||
        src_ws_idx < 0 || src_ws_idx >= (int)workspaces->len ||
        dest_ws_idx < 0 || dest_ws_idx >= (int)workspaces->len)
        return FALSE;

    Workspace *src_ws = g_ptr_array_index(workspaces, src_ws_idx);
    Workspace *dest_ws = g_ptr_array_index(workspaces, dest_ws_idx);
    if (!src_ws || !dest_ws || !src_ws->pane_notebooks || !dest_ws->pane_notebooks)
        return FALSE;

    if (src_pane_idx < 0 || src_pane_idx >= (int)src_ws->pane_notebooks->len)
        return FALSE;
    if (dest_pane_idx < 0 || dest_pane_idx >= (int)dest_ws->pane_notebooks->len)
        return FALSE;

    GtkNotebook *src_nb = g_ptr_array_index(src_ws->pane_notebooks, src_pane_idx);
    GtkNotebook *dest_nb = g_ptr_array_index(dest_ws->pane_notebooks, dest_pane_idx);
    if (!GTK_IS_NOTEBOOK(src_nb) || !GTK_IS_NOTEBOOK(dest_nb))
        return FALSE;
    if (src_tab_idx < 0 || src_tab_idx >= gtk_notebook_get_n_pages(src_nb))
        return FALSE;

    GtkWidget *terminal = notebook_terminal_at(src_nb, src_tab_idx);
    if (!terminal)
        return FALSE;

    return move_terminal_to_notebook(src_ws, src_nb, terminal, dest_ws, dest_nb);
}

gboolean
workspace_select_tab(int ws_idx, int pane_idx, int tab_idx)
{
    Workspace *ws;
    GtkNotebook *nb;
    GtkWidget *terminal;

    if (!workspaces || ws_idx < 0 || ws_idx >= (int)workspaces->len)
        return FALSE;

    ws = g_ptr_array_index(workspaces, ws_idx);
    if (!ws || !ws->pane_notebooks || pane_idx < 0 ||
        pane_idx >= (int)ws->pane_notebooks->len)
        return FALSE;

    nb = g_ptr_array_index(ws->pane_notebooks, pane_idx);
    if (!GTK_IS_NOTEBOOK(nb) || tab_idx < 0 ||
        tab_idx >= gtk_notebook_get_n_pages(nb))
        return FALSE;

    if (ws_idx != current_workspace && g_terminal_stack && g_workspace_list)
        workspace_switch(ws_idx, g_terminal_stack, g_workspace_list);

    gtk_notebook_set_current_page(nb, tab_idx);
    terminal = notebook_terminal_at(nb, tab_idx);
    focus_terminal_page(terminal);
    focus_terminal_page_later(terminal);
    return TRUE;
}

void workspace_start_tab_rename(Workspace *ws) {
    if (!ws) return;
    GtkNotebook *nb = workspace_get_focused_pane(ws);
    if (!nb || !GTK_IS_NOTEBOOK(nb)) return;
    int pg = gtk_notebook_get_current_page(nb);
    if (pg < 0) return;
    GtkWidget *child = gtk_notebook_get_nth_page(nb, pg);
    if (!child) return;
    GtkWidget *tab_w = gtk_notebook_get_tab_label(nb, child);
    if (!tab_w) return;
    RenameData *rd = g_object_get_data(G_OBJECT(tab_w), "rename-data");
    if (!rd) return;
    start_rename(rd);
}

/* ── Workspace sidebar row ──────────────────────────────────────── */

static GtkWidget *create_workspace_row(Workspace *ws, int ws_idx) {
    GtkWidget *inner_label = NULL;
    GtkWidget *box = create_editable_tab_label(
        ws->name, NULL, ws, TRUE, &inner_label);
    gtk_widget_add_css_class(box, "sidebar-row");
    ws->sidebar_label = inner_label;
    /* Prevent sidebar from expanding — ellipsize long paths */
    gtk_label_set_ellipsize(GTK_LABEL(inner_label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(inner_label), 22);

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
    session_queue_save();
}

/* ── Native DnD: close empty pane after tab is dragged out ──────── */

static gboolean
close_pane_idle_cb(gpointer user_data)
{
    GtkNotebook *notebook = GTK_NOTEBOOK(user_data);
    Workspace *ws = g_object_get_data(G_OBJECT(notebook), "workspace-ptr");

    if (ws && GTK_IS_NOTEBOOK(notebook) &&
        gtk_notebook_get_n_pages(notebook) == 0 &&
        ws->pane_notebooks && ws->pane_notebooks->len > 1) {
        workspace_close_pane(ws, notebook);
    }
    g_object_unref(notebook);
    return G_SOURCE_REMOVE;
}

static void
on_notebook_page_removed(GtkNotebook *notebook, GtkWidget *child,
                         guint page_num, gpointer user_data)
{
    (void)child; (void)page_num;
    Workspace *ws = user_data;

    /* During shutdown, GTK removes pages as it destroys widgets.
     * Don't modify the terminals array or close panes — session was
     * already saved before shutdown started. */
    if (app_shutting_down) return;

    if (gtk_notebook_get_n_pages(notebook) == 0 &&
        ws && ws->pane_notebooks && ws->pane_notebooks->len > 1) {
        g_object_set_data(G_OBJECT(notebook), "workspace-ptr", ws);
        g_object_ref(notebook);
        g_idle_add(close_pane_idle_cb, notebook);
    }
}

static void
on_notebook_page_added(GtkNotebook *notebook, GtkWidget *child,
                       guint page_num, gpointer user_data)
{
    (void)page_num;
    Workspace *ws = user_data;

    if (ws) {
        workspace_set_active_pane(ws, notebook);
        GtkWidget *terminal = page_linked_terminal(child);
        if (terminal && GHOSTTY_IS_TERMINAL(terminal))
            focus_terminal_page_later(terminal);
    }
}

static void
on_notebook_pointer_enter(GtkEventControllerMotion *controller,
                          double x, double y, gpointer user_data)
{
    Workspace *ws = user_data;
    GtkWidget *widget = gtk_event_controller_get_widget(
        GTK_EVENT_CONTROLLER(controller));
    (void)x;
    (void)y;

    if (!app_settings_get_focus_on_hover())
        return;

    if (!hover_focus_should_enter())
        return;

    if (ws && GTK_IS_NOTEBOOK(widget))
        workspace_set_active_pane(ws, GTK_NOTEBOOK(widget));
}

static void
on_notebook_pointer_motion(GtkEventControllerMotion *controller,
                           double x, double y, gpointer user_data)
{
    (void)controller;
    (void)x;
    (void)y;
    (void)user_data;
    hover_focus_note_pointer_motion();
}

static void
on_notebook_pressed(GtkGestureClick *gesture, int n_press,
                    double x, double y, gpointer user_data)
{
    Workspace *ws = user_data;
    GtkWidget *widget = gtk_event_controller_get_widget(
        GTK_EVENT_CONTROLLER(gesture));
    (void)n_press;
    (void)x;
    (void)y;

    if (ws && GTK_IS_NOTEBOOK(widget))
        workspace_set_active_pane(ws, GTK_NOTEBOOK(widget));
}

/* Helper: create a notebook for a new pane and wire up the "+" button. */
static GtkWidget *
create_pane_notebook(Workspace *ws, ghostty_app_t app)
{
    GtkWidget *notebook = gtk_notebook_new();
    char *pane_id = g_strdup_printf("pane-%" G_GUINT64_FORMAT,
                                    next_pane_serial++);
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(notebook), TRUE);
    gtk_notebook_set_show_border(GTK_NOTEBOOK(notebook), FALSE);
    g_object_set_data_full(G_OBJECT(notebook), "pane-id", pane_id, g_free);

    /* "+" button */
    GtkWidget *add_btn = gtk_button_new_with_label("+");
    g_object_set_data(G_OBJECT(add_btn), "workspace", ws);
    g_object_set_data(G_OBJECT(add_btn), "app", app);
    g_signal_connect(add_btn, "clicked", G_CALLBACK(on_ws_add_tab_clicked), NULL);
    gtk_notebook_set_action_widget(GTK_NOTEBOOK(notebook), add_btn, GTK_PACK_END);
    gtk_widget_set_visible(add_btn, TRUE);

    /* Native notebook DnD gives proper tab tear/reorder visuals. */
    gtk_notebook_set_group_name(GTK_NOTEBOOK(notebook), "prettymux-panes");
    g_object_set_data(G_OBJECT(notebook), "workspace-ptr", ws);

    /* Track tabs being dragged in/out for workspace terminal arrays */
    g_signal_connect(notebook, "page-removed",
                     G_CALLBACK(on_notebook_page_removed), ws);
    g_signal_connect(notebook, "page-added",
                     G_CALLBACK(on_notebook_page_added), ws);

    /* On tab switch, clear activity on the newly selected terminal */
    g_signal_connect(notebook, "switch-page",
                     G_CALLBACK(on_notebook_switch_page), ws);

    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion",
                     G_CALLBACK(on_notebook_pointer_motion), ws);
    g_signal_connect(motion, "enter",
                     G_CALLBACK(on_notebook_pointer_enter), ws);
    gtk_widget_add_controller(notebook, motion);

    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
    g_signal_connect(click, "pressed",
                     G_CALLBACK(on_notebook_pressed), ws);
    gtk_widget_add_controller(notebook, GTK_EVENT_CONTROLLER(click));

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
    ws->active_pane = NULL;
    ws->sidebar_label = NULL;

    ws->overlay = gtk_overlay_new();
    ws->notebook = create_pane_notebook(ws, app);
    gtk_overlay_set_child(GTK_OVERLAY(ws->overlay), ws->notebook);
    g_ptr_array_add(ws->pane_notebooks, ws->notebook);
    ws->active_pane = GTK_NOTEBOOK(ws->notebook);
    ws->container = ws->overlay;

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

    if (ws->pane_notebooks) {
        for (guint i = 0; i < ws->pane_notebooks->len; i++) {
            GtkNotebook *notebook = g_ptr_array_index(ws->pane_notebooks, i);

            if (!GTK_IS_NOTEBOOK(notebook))
                continue;

            g_signal_handlers_disconnect_by_data(notebook, ws);
            g_object_set_data(G_OBJECT(notebook), "workspace-ptr", NULL);
        }
    }

    ws->active_pane = NULL;
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
    port_scanner_set_active_workspace(index);

    /* Bug 9 fix: use the workspace's container widget directly instead of
     * building a name string.  workspace_remove doesn't renumber stack
     * children, so "ws-N" can become stale after deletions. */
    Workspace *target = g_ptr_array_index(workspaces, index);
    gtk_stack_set_visible_child(GTK_STACK(terminal_stack), target->container);

    GtkListBoxRow *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(workspace_list), index);
    if (row)
        gtk_list_box_select_row(GTK_LIST_BOX(workspace_list), row);

    /* Always land on the first tab of the first pane when switching
     * workspaces, including freshly-created ones. */
    Workspace *ws = g_ptr_array_index(workspaces, index);
    if (ws)
        workspace_focus_first_terminal(ws);
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
                    if (g_ptr_array_index(ws->pane_notebooks, i) == w) {
                        ws->active_pane = GTK_NOTEBOOK(w);
                        return GTK_NOTEBOOK(w);
                    }
                }
            } else if (GHOSTTY_IS_TERMINAL(w)) {
                GtkNotebook *nb = terminal_parent_notebook(w);
                if (workspace_has_pane(ws, nb)) {
                    ws->active_pane = nb;
                    return nb;
                }
            }
        }
    }

    if (ws->active_pane) {
        for (guint i = 0; i < ws->pane_notebooks->len; i++) {
            if (g_ptr_array_index(ws->pane_notebooks, i) == ws->active_pane)
                return ws->active_pane;
        }
        ws->active_pane = NULL;
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
GtkNotebook *
workspace_split_pane_target(Workspace *ws, GtkNotebook *source_nb,
                            GtkOrientation orientation,
                            ghostty_app_t app)
{
    const char *cwd = NULL;

    if (!ws || !source_nb)
        return NULL;

    {
        int page = gtk_notebook_get_current_page(source_nb);
        GtkWidget *terminal = notebook_terminal_at(source_nb, page);
        if (GHOSTTY_IS_TERMINAL(terminal))
            cwd = ghostty_terminal_get_cwd(GHOSTTY_TERMINAL(terminal));
    }

    GtkWidget *source_widget = GTK_WIDGET(source_nb);
    GtkWidget *parent = gtk_widget_get_parent(source_widget);

    /* Create the new pane notebook */
    GtkWidget *new_nb = create_pane_notebook(ws, app);
    g_ptr_array_add(ws->pane_notebooks, new_nb);
    workspace_set_active_pane(ws, GTK_NOTEBOOK(new_nb));

    /* Create the new paned container */
    GtkWidget *paned = gtk_paned_new(orientation);
    gtk_widget_set_hexpand(paned, TRUE);
    gtk_widget_set_vexpand(paned, TRUE);

    /* Connect resize overlay to show dimensions while dragging */
    resize_overlay_connect_paned(GTK_PANED(paned));
    g_signal_connect(paned, "notify::position",
                     G_CALLBACK(workspace_on_paned_position_notify), NULL);

    /* Reparenting through GtkPaned drops the parent's ref immediately.
     * Hold our own ref so nested splits don't destroy the source pane
     * before we attach it to the new paned container. */
    g_object_ref(source_widget);

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
        gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);
        gtk_paned_set_shrink_end_child(GTK_PANED(paned), FALSE);

    } else if (GTK_IS_OVERLAY(parent)) {
        gtk_overlay_set_child(GTK_OVERLAY(parent), NULL);

        gtk_paned_set_start_child(GTK_PANED(paned), source_widget);
        gtk_paned_set_end_child(GTK_PANED(paned), new_nb);
        gtk_paned_set_resize_start_child(GTK_PANED(paned), TRUE);
        gtk_paned_set_resize_end_child(GTK_PANED(paned), TRUE);
        gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);
        gtk_paned_set_shrink_end_child(GTK_PANED(paned), FALSE);
        gtk_overlay_set_child(GTK_OVERLAY(parent), paned);
    } else {
        /* Parent is neither GtkPaned nor GtkStack -- abort split */
        g_ptr_array_remove(ws->pane_notebooks, new_nb);
        g_object_unref(source_widget);
        return NULL;
    }

    g_object_unref(source_widget);

    gtk_widget_set_visible(paned, TRUE);
    gtk_widget_set_visible(new_nb, TRUE);

    /* Set paned to 50% once it has a size */
    g_object_ref(paned);
    g_idle_add(set_paned_half, paned);

    /* Add a terminal to the new pane */
    workspace_add_terminal_to_notebook_cwd(
        ws,
        GTK_NOTEBOOK(new_nb),
        app,
        (cwd && cwd[0]) ? cwd : NULL);

    /* Focus the new terminal's GtkGLArea so it receives key events */
    {
        int last = gtk_notebook_get_n_pages(GTK_NOTEBOOK(new_nb)) - 1;
        if (last >= 0) {
            GtkWidget *terminal = notebook_terminal_at(GTK_NOTEBOOK(new_nb), last);
            if (terminal)
                focus_terminal_page_later(terminal);
        }
    }

    return GTK_NOTEBOOK(new_nb);
}

void
workspace_split_pane(Workspace *ws, GtkOrientation orientation,
                     ghostty_app_t app)
{
    if (!ws)
        return;

    workspace_split_pane_target(ws, workspace_get_focused_pane(ws),
                                orientation, app);
}

static int
workspace_equalize_leaf_count(GtkWidget *widget,
                              gboolean filter_orientation,
                              GtkOrientation orientation)
{
    if (!GTK_IS_PANED(widget))
        return 1;

    GtkOrientation current =
        gtk_orientable_get_orientation(GTK_ORIENTABLE(widget));
    if (filter_orientation && current != orientation)
        return 1;

    GtkWidget *start = gtk_paned_get_start_child(GTK_PANED(widget));
    GtkWidget *end = gtk_paned_get_end_child(GTK_PANED(widget));
    int start_count = start
        ? workspace_equalize_leaf_count(start, filter_orientation, orientation)
        : 0;
    int end_count = end
        ? workspace_equalize_leaf_count(end, filter_orientation, orientation)
        : 0;
    int total = start_count + end_count;
    return total > 0 ? total : 1;
}

static void
workspace_equalize_widget(GtkWidget *widget,
                          gboolean filter_orientation,
                          GtkOrientation orientation)
{
    if (!GTK_IS_PANED(widget))
        return;

    GtkPaned *paned = GTK_PANED(widget);
    GtkWidget *start = gtk_paned_get_start_child(paned);
    GtkWidget *end = gtk_paned_get_end_child(paned);
    GtkOrientation current =
        gtk_orientable_get_orientation(GTK_ORIENTABLE(widget));

    if ((!filter_orientation || current == orientation) && start && end) {
        int start_count =
            workspace_equalize_leaf_count(start, filter_orientation, orientation);
        int end_count =
            workspace_equalize_leaf_count(end, filter_orientation, orientation);
        int total = start_count + end_count;
        int size = (current == GTK_ORIENTATION_HORIZONTAL)
            ? gtk_widget_get_width(widget)
            : gtk_widget_get_height(widget);

        if (total > 0 && size > 10) {
            int pos = (int)((double)size * ((double)start_count / (double)total));
            gtk_paned_set_position(paned, pos);
        }
    }

    if (start)
        workspace_equalize_widget(start, filter_orientation, orientation);
    if (end)
        workspace_equalize_widget(end, filter_orientation, orientation);
}

gboolean
workspace_equalize_splits(Workspace *ws, const char *orientation_name)
{
    gboolean filter_orientation = FALSE;
    GtkOrientation orientation = GTK_ORIENTATION_HORIZONTAL;
    GtkWidget *root;

    if (!ws || !GTK_IS_OVERLAY(ws->overlay))
        return FALSE;

    if (orientation_name && orientation_name[0]) {
        filter_orientation = TRUE;
        if (g_strcmp0(orientation_name, "vertical") == 0)
            orientation = GTK_ORIENTATION_VERTICAL;
        else if (g_strcmp0(orientation_name, "horizontal") == 0)
            orientation = GTK_ORIENTATION_HORIZONTAL;
        else
            return FALSE;
    }

    root = gtk_overlay_get_child(GTK_OVERLAY(ws->overlay));
    if (!root)
        return FALSE;

    workspace_equalize_widget(root, filter_orientation, orientation);
    return TRUE;
}

gboolean
workspace_resize_pane_percent(Workspace *ws, GtkNotebook *pane,
                              char axis, double percent)
{
    GtkWidget *pane_widget;
    GtkWidget *parent;
    GtkPaned *paned;
    GtkOrientation orientation;
    int size;
    int position;

    if (!ws || !GTK_IS_NOTEBOOK(pane) || percent <= 0.0 || percent >= 100.0)
        return FALSE;

    pane_widget = GTK_WIDGET(pane);
    parent = gtk_widget_get_parent(pane_widget);
    if (!GTK_IS_PANED(parent))
        return FALSE;

    paned = GTK_PANED(parent);
    orientation = gtk_orientable_get_orientation(GTK_ORIENTABLE(parent));
    if ((axis == 'x' && orientation != GTK_ORIENTATION_HORIZONTAL) ||
        (axis == 'y' && orientation != GTK_ORIENTATION_VERTICAL))
        return FALSE;

    size = (orientation == GTK_ORIENTATION_HORIZONTAL)
        ? gtk_widget_get_width(parent)
        : gtk_widget_get_height(parent);
    if (size <= 10)
        return FALSE;

    position = (int)((percent / 100.0) * (double)size);
    if (gtk_paned_get_end_child(paned) == pane_widget)
        position = size - position;

    gtk_paned_set_position(paned, position);
    return TRUE;
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
        GtkWidget *terminal = notebook_terminal_at(best, pg);
        if (terminal)
            focus_terminal_page_later(terminal);
    }
}

/* ── Close pane ──────────────────────────────────────────────── */

void
workspace_close_pane(Workspace *ws, GtkNotebook *pane)
{
    if (!ws || !pane) return;
    if (!GTK_IS_NOTEBOOK(pane)) return;
    if (!ws->pane_notebooks || ws->pane_notebooks->len <= 1) return;

    GtkWidget *pane_widget = GTK_WIDGET(pane);
    GtkWidget *parent = gtk_widget_get_parent(pane_widget);

    if (!GTK_IS_PANED(parent)) return;

    GtkPaned *parent_paned = GTK_PANED(parent);

    /* Verify pane is actually a child of parent_paned */
    GtkWidget *start = gtk_paned_get_start_child(parent_paned);
    GtkWidget *end_c = gtk_paned_get_end_child(parent_paned);
    if (pane_widget != start && pane_widget != end_c) return;

    GtkWidget *grandparent = gtk_widget_get_parent(GTK_WIDGET(parent_paned));

    /* Verify grandparent is a valid container type before any detach */
    if (!GTK_IS_PANED(grandparent) && !GTK_IS_OVERLAY(grandparent)) return;

    /* Determine the sibling (the other child of the paned). */
    GtkWidget *sibling = (start == pane_widget) ? end_c : start;

    if (!sibling) return;

    /* Remove terminals that belong to the closing pane from ws->terminals */
    {
        int n_pages = gtk_notebook_get_n_pages(pane);
        int i;
        for (i = 0; i < n_pages; i++) {
            GtkWidget *child = gtk_notebook_get_nth_page(pane, i);
            GtkWidget *terminal = page_linked_terminal(child);
            if (terminal) {
                g_ptr_array_remove(ws->terminals, terminal);
                if (ws->overlay)
                    gtk_overlay_remove_overlay(GTK_OVERLAY(ws->overlay), terminal);
            }
        }
    }

    /* Determine which side of grandparent holds parent_paned */
    gboolean is_start_in_gp = FALSE;
    if (GTK_IS_PANED(grandparent))
        is_start_in_gp = (gtk_paned_get_start_child(GTK_PANED(grandparent))
                          == GTK_WIDGET(parent_paned));

    /* Ref sibling so it survives reparenting */
    g_object_ref(sibling);

    /* Detach both children from parent_paned */
    gtk_paned_set_start_child(parent_paned, NULL);
    gtk_paned_set_end_child(parent_paned, NULL);

    /* Now remove the (empty) parent_paned from its grandparent
     * and replace it with the sibling */
    if (GTK_IS_PANED(grandparent)) {
        /* Remove the empty paned from grandparent */
        if (is_start_in_gp)
            gtk_paned_set_start_child(GTK_PANED(grandparent), NULL);
        else
            gtk_paned_set_end_child(GTK_PANED(grandparent), NULL);

        /* Insert sibling in its place */
        if (is_start_in_gp)
            gtk_paned_set_start_child(GTK_PANED(grandparent), sibling);
        else
            gtk_paned_set_end_child(GTK_PANED(grandparent), sibling);

    } else if (GTK_IS_OVERLAY(grandparent)) {
        gtk_overlay_set_child(GTK_OVERLAY(grandparent), sibling);

        if (GTK_IS_NOTEBOOK(sibling))
            ws->notebook = sibling;
    }

    g_object_unref(sibling);

    /* Remove the pane from pane_notebooks AFTER successful reparent */
    g_ptr_array_remove(ws->pane_notebooks, pane);

    /* Update ws->notebook if it was the closed pane */
    if (ws->notebook == GTK_WIDGET(pane) && ws->pane_notebooks->len > 0)
        ws->notebook = g_ptr_array_index(ws->pane_notebooks, 0);
    if (ws->active_pane == pane)
        ws->active_pane = GTK_IS_NOTEBOOK(sibling) ? GTK_NOTEBOOK(sibling) : NULL;

    /* Focus the sibling's first terminal */
    if (GTK_IS_NOTEBOOK(sibling)) {
        int pg = gtk_notebook_get_current_page(GTK_NOTEBOOK(sibling));
        if (pg >= 0) {
            GtkWidget *terminal = notebook_terminal_at(GTK_NOTEBOOK(sibling), pg);
            if (terminal)
                focus_terminal_page_later(terminal);
        }
    }
}

/* (activity helpers are defined earlier in this file) */

/* ── Notebook switch-page: clear activity on focused terminal ──── */

static void
on_notebook_switch_page(GtkNotebook *nb, GtkWidget *page,
                        guint page_num, gpointer user_data)
{
    (void)nb;
    Workspace *ws = user_data;

    workspace_set_active_pane(ws, nb);
    GtkWidget *terminal = page_linked_terminal(page);
    if (terminal && GHOSTTY_IS_TERMINAL(terminal)) {
        const char *cwd = ghostty_terminal_get_cwd(GHOSTTY_TERMINAL(terminal));

        if (ws && cwd && cwd[0]) {
            snprintf(ws->cwd, sizeof(ws->cwd), "%s", cwd);
            workspace_detect_git(ws);
        }

        focus_terminal_page(terminal);
        focus_terminal_page_later(terminal);
        ghostty_terminal_clear_activity(GHOSTTY_TERMINAL(terminal));
        workspace_clear_tab_notification(nb, (int)page_num);
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

    start_rename(rd);
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
