#include "sidebar_ui.h"

#include <gtk/gtk.h>
#include <string.h>

#include "app_state.h"
#include "notifications.h"
#include "pane_move_overlay.h"
#include "session.h"
#include "workspace.h"

#define SIDEBAR_ROW_REVEAL_DURATION_USEC (170 * 1000)
#define SIDEBAR_ROW_REVEAL_START_MARGIN  6

typedef struct {
    GtkWidget *row;
    guint      tick_id;
    gint64     start_usec;
    gboolean   completed;
} SidebarRowRevealAnim;

static void
sidebar_ui_finish_row_reveal(GtkWidget *row, SidebarRowRevealAnim *anim)
{
    if (!GTK_IS_WIDGET(row) || !anim)
        return;

    if (anim->tick_id) {
        gtk_widget_remove_tick_callback(row, anim->tick_id);
        anim->tick_id = 0;
    }
    anim->start_usec = 0;
    anim->completed = TRUE;

    gtk_widget_set_opacity(row, 1.0);
    gtk_widget_set_margin_top(row, 0);
}

static gboolean
sidebar_ui_row_reveal_tick_cb(GtkWidget *widget,
                              GdkFrameClock *frame_clock,
                              gpointer user_data)
{
    SidebarRowRevealAnim *anim = user_data;
    gint64 now_usec;
    double inv;
    double progress;
    double eased;

    if (!anim || anim->row != widget || !GTK_IS_WIDGET(widget))
        return G_SOURCE_REMOVE;

    if (!gtk_widget_get_mapped(widget)) {
        sidebar_ui_finish_row_reveal(widget, anim);
        return G_SOURCE_REMOVE;
    }

    now_usec = gdk_frame_clock_get_frame_time(frame_clock);
    if (anim->start_usec <= 0)
        anim->start_usec = now_usec;

    progress = (double)(now_usec - anim->start_usec) /
               (double)SIDEBAR_ROW_REVEAL_DURATION_USEC;
    progress = CLAMP(progress, 0.0, 1.0);
    inv = 1.0 - progress;
    eased = 1.0 - (inv * inv * inv);

    gtk_widget_set_opacity(widget, eased);
    gtk_widget_set_margin_top(widget,
                              (int)(((1.0 - eased) *
                                     SIDEBAR_ROW_REVEAL_START_MARGIN) + 0.5));

    if (progress >= 1.0) {
        sidebar_ui_finish_row_reveal(widget, anim);
        return G_SOURCE_REMOVE;
    }

    return G_SOURCE_CONTINUE;
}

static void
sidebar_ui_row_reveal_anim_free(gpointer data)
{
    SidebarRowRevealAnim *anim = data;

    if (!anim)
        return;
    if (GTK_IS_WIDGET(anim->row) && anim->tick_id)
        gtk_widget_remove_tick_callback(anim->row, anim->tick_id);
    g_free(anim);
}

static void
sidebar_ui_on_row_mapped(GtkWidget *row, gpointer user_data)
{
    SidebarRowRevealAnim *anim = user_data;

    if (!anim || !GTK_IS_WIDGET(row))
        return;
    if (anim->completed)
        return;
    if (anim->tick_id)
        return;

    anim->start_usec = 0;
    anim->tick_id = gtk_widget_add_tick_callback(
        row, sidebar_ui_row_reveal_tick_cb, anim, NULL);
}

static void
sidebar_ui_start_row_reveal(GtkWidget *row)
{
    SidebarRowRevealAnim *anim;

    if (!GTK_IS_WIDGET(row))
        return;
    if (g_object_get_data(G_OBJECT(row), "sidebar-row-reveal-anim"))
        return;

    anim = g_new0(SidebarRowRevealAnim, 1);
    anim->row = row;
    g_object_set_data_full(G_OBJECT(row),
                           "sidebar-row-reveal-anim",
                           anim,
                           sidebar_ui_row_reveal_anim_free);

    gtk_widget_set_opacity(row, 0.0);
    gtk_widget_set_margin_top(row, SIDEBAR_ROW_REVEAL_START_MARGIN);
    g_signal_connect(row, "map", G_CALLBACK(sidebar_ui_on_row_mapped), anim);
}

/* Pending workspace activation, deferred until the double-click
 * window (~250ms) closes. If a double-click rename starts before the
 * timeout fires, sidebar_ui_cancel_pending_activate() cancels it so
 * the terminal-focus pull doesn't steal focus from the rename entry. */
static guint   pending_activate_source_id;
static int     pending_activate_index = -1;

static gboolean
deferred_activate_cb(gpointer user_data)
{
    (void)user_data;
    pending_activate_source_id = 0;
    int idx = pending_activate_index;
    pending_activate_index = -1;
    if (idx >= 0)
        workspace_switch(idx, ui.terminal_stack, ui.workspace_list);
    session_queue_save();
    return G_SOURCE_REMOVE;
}

void
sidebar_ui_cancel_pending_activate(void)
{
    if (pending_activate_source_id) {
        g_source_remove(pending_activate_source_id);
        pending_activate_source_id = 0;
        pending_activate_index = -1;
    }
}

static void
on_workspace_row_activated(GtkListBox *list, GtkListBoxRow *row, gpointer user_data)
{
    GtkWidget *child;
    GtkWidget *header_box;
    GtkWidget *rename_entry;
    gboolean rename_in_progress = FALSE;
    gboolean rename_suppressed = FALSE;

    (void)list;
    (void)user_data;

    child = gtk_list_box_row_get_child(row);
    header_box = child ? g_object_get_data(G_OBJECT(child), "header-box") : NULL;
    rename_in_progress = header_box &&
        g_object_get_data(G_OBJECT(header_box), "rename-in-progress");
    rename_suppressed = header_box &&
        g_object_get_data(G_OBJECT(header_box), "rename-activate-suppressed");
    rename_entry = header_box
        ? g_object_get_data(G_OBJECT(header_box), "rename-entry") : NULL;
    if (!rename_entry && child)
        rename_entry = g_object_get_data(G_OBJECT(child), "rename-entry");
    if (GTK_IS_WIDGET(rename_entry)) {
        gtk_widget_grab_focus(rename_entry);
        return;
    }
    if (rename_in_progress || rename_suppressed)
        return;

    /* Defer the workspace activation by the double-click window so a
     * pending rename gesture can cancel it before the row's focus
     * pulls focus to the first ghostty terminal — otherwise the
     * rename entry never wins focus. start_rename() (triggered from
     * the second click in CAPTURE phase) calls
     * sidebar_ui_cancel_pending_activate() which removes this
     * timeout. */
    int target_idx = gtk_list_box_row_get_index(row);
    sidebar_ui_cancel_pending_activate();
    pending_activate_index = target_idx;
    pending_activate_source_id = g_timeout_add(280, deferred_activate_cb,
                                                NULL);
}

static gboolean
workspace_row_matches_query(Workspace *ws, const char *query)
{
    g_autofree char *needle = NULL;
    g_autofree char *name = NULL;
    g_autofree char *cwd = NULL;
    g_autofree char *branch = NULL;

    if (!ws || !query || !query[0])
        return TRUE;

    needle = g_utf8_strdown(query, -1);
    name = g_utf8_strdown(ws->name, -1);

    cwd = ws->cwd[0] ? g_utf8_strdown(ws->cwd, -1) : NULL;
    branch = ws->git_branch[0] ? g_utf8_strdown(ws->git_branch, -1) : NULL;

    return (name && strstr(name, needle)) ||
           (cwd && strstr(cwd, needle)) ||
           (branch && strstr(branch, needle));
}

static gboolean
workspace_list_filter_func(GtkListBoxRow *row, gpointer user_data)
{
    GtkWidget *search = GTK_WIDGET(user_data);
    GtkWidget *child;
    Workspace *ws;
    const char *query;

    if (!GTK_IS_EDITABLE(search))
        return TRUE;

    query = gtk_editable_get_text(GTK_EDITABLE(search));
    if (!query || !query[0])
        return TRUE;

    child = gtk_list_box_row_get_child(row);
    ws = child ? g_object_get_data(G_OBJECT(child), "workspace") : NULL;
    return workspace_row_matches_query(ws, query);
}

static void
on_workspace_search_changed(GtkSearchEntry *entry, gpointer user_data)
{
    GtkListBox *list = GTK_LIST_BOX(user_data);
    (void)entry;

    if (GTK_IS_LIST_BOX(list))
        gtk_list_box_invalidate_filter(list);
}

static void
on_add_workspace_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    (void)user_data;
    workspace_add(ui.terminal_stack, ui.workspace_list, g_ghostty_app);
}

GtkWidget *
sidebar_ui_build_workspace_card(GtkWidget  *header_box,
                                GtkWidget **out_meta_label,
                                GtkWidget **out_status_label,
                                GtkWidget **out_status_entries_box,
                                GtkWidget **out_ports_label,
                                GtkWidget **out_progress_label,
                                GtkWidget **out_structure_label,
                                GtkWidget **out_badge)
{
    GtkWidget *card;
    GtkWidget *details;
    GtkWidget *aux_row;
    GtkWidget *ports;
    GtkWidget *progress;
    GtkWidget *structure;
    GtkWidget *meta;
    GtkWidget *status;
    GtkWidget *status_entries;
    GtkWidget *badge;

    card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(card, "sidebar-card");

    badge = gtk_label_new("\342\227\217");
    gtk_widget_add_css_class(badge, "sidebar-badge");
    gtk_widget_set_visible(badge, FALSE);
    gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(header_box), badge);

    gtk_box_append(GTK_BOX(card), header_box);

    details = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(details, "sidebar-card-details");

    meta = gtk_label_new("");
    gtk_widget_add_css_class(meta, "sidebar-meta");
    gtk_widget_add_css_class(meta, "sidebar-branch-cwd");
    gtk_label_set_xalign(GTK_LABEL(meta), 0);
    gtk_label_set_ellipsize(GTK_LABEL(meta), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(meta), 26);
    gtk_widget_set_visible(meta, FALSE);

    status_entries = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(status_entries, "sidebar-status-section");
    gtk_widget_set_visible(status_entries, FALSE);
    gtk_box_append(GTK_BOX(details), status_entries);

    status = gtk_label_new("");
    gtk_widget_add_css_class(status, "sidebar-status");
    gtk_widget_add_css_class(status, "sidebar-notification-preview");
    gtk_label_set_xalign(GTK_LABEL(status), 0);
    gtk_label_set_ellipsize(GTK_LABEL(status), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(status), 30);
    gtk_widget_set_visible(status, FALSE);
    gtk_box_append(GTK_BOX(details), status);

    gtk_box_append(GTK_BOX(details), meta);

    aux_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(aux_row, "sidebar-aux-row");

    structure = gtk_label_new("");
    gtk_widget_add_css_class(structure, "sidebar-meta");
    gtk_widget_add_css_class(structure, "sidebar-structure-indicator");
    gtk_label_set_xalign(GTK_LABEL(structure), 0);
    gtk_label_set_ellipsize(GTK_LABEL(structure), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(structure), 10);
    gtk_widget_set_visible(structure, FALSE);
    gtk_box_append(GTK_BOX(aux_row), structure);

    ports = gtk_label_new("");
    gtk_widget_add_css_class(ports, "sidebar-meta");
    gtk_widget_add_css_class(ports, "sidebar-ports");
    gtk_label_set_xalign(GTK_LABEL(ports), 0);
    gtk_label_set_ellipsize(GTK_LABEL(ports), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(ports), 16);
    gtk_widget_set_visible(ports, FALSE);
    gtk_box_append(GTK_BOX(aux_row), ports);

    progress = gtk_label_new("");
    gtk_widget_add_css_class(progress, "sidebar-meta");
    gtk_widget_add_css_class(progress, "sidebar-progress");
    gtk_widget_add_css_class(progress, "sidebar-status");
    gtk_label_set_xalign(GTK_LABEL(progress), 0);
    gtk_label_set_ellipsize(GTK_LABEL(progress), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(progress), 16);
    gtk_widget_set_visible(progress, FALSE);
    gtk_box_append(GTK_BOX(aux_row), progress);

    gtk_box_append(GTK_BOX(details), aux_row);

    gtk_box_append(GTK_BOX(card), details);

    g_object_set_data(G_OBJECT(card), "header-box", header_box);
    sidebar_ui_start_row_reveal(card);

    if (out_meta_label)  *out_meta_label  = meta;
    if (out_status_label) *out_status_label = status;
    if (out_status_entries_box) *out_status_entries_box = status_entries;
    if (out_ports_label) *out_ports_label = ports;
    if (out_progress_label) *out_progress_label = progress;
    if (out_structure_label) *out_structure_label = structure;
    if (out_badge)        *out_badge        = badge;

    return card;
}

void
sidebar_ui_show_move_to_window_menu(Workspace *workspace)
{
    int ws_idx;

    if (!workspace || !GTK_IS_OVERLAY(ui.overlay))
        return;

    ws_idx = workspace_get_index(workspace);
    if (ws_idx < 0)
        return;

    pane_move_overlay_toggle_workspace_targets(GTK_OVERLAY(ui.overlay),
                                               ui.terminal_stack,
                                               ui.workspace_list,
                                               ws_idx);
}

void
sidebar_ui_build(void)
{
    GtkWidget *bottom_box;
    GtkWidget *scroll;
    GtkWidget *btn;

    ui.sidebar_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(ui.sidebar_box, "sidebar");
    gtk_widget_set_size_request(ui.sidebar_box, 180, -1);

    ui.workspace_search = gtk_search_entry_new();
    g_object_set(G_OBJECT(ui.workspace_search),
                 "placeholder-text", "Search workspaces",
                 NULL);
    gtk_widget_set_margin_start(ui.workspace_search, 8);
    gtk_widget_set_margin_end(ui.workspace_search, 8);
    gtk_widget_set_margin_top(ui.workspace_search, 8);
    gtk_widget_set_margin_bottom(ui.workspace_search, 4);
    gtk_box_append(GTK_BOX(ui.sidebar_box), ui.workspace_search);

    ui.workspace_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(ui.workspace_list),
                                    GTK_SELECTION_SINGLE);
    g_signal_connect(ui.workspace_list, "row-activated",
                     G_CALLBACK(on_workspace_row_activated), NULL);
    gtk_list_box_set_filter_func(GTK_LIST_BOX(ui.workspace_list),
                                 workspace_list_filter_func,
                                 ui.workspace_search, NULL);
    g_signal_connect(ui.workspace_search, "search-changed",
                     G_CALLBACK(on_workspace_search_changed),
                     ui.workspace_list);

    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), ui.workspace_list);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(ui.sidebar_box), scroll);

    bottom_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_start(bottom_box, 8);
    gtk_widget_set_margin_end(bottom_box, 8);
    gtk_widget_set_margin_bottom(bottom_box, 8);
    gtk_widget_set_margin_top(bottom_box, 4);

    ui.bell_button = gtk_button_new_with_label("\360\237\224\224");
    g_signal_connect(ui.bell_button, "clicked",
                     G_CALLBACK(notifications_on_bell_button_clicked), NULL);
    gtk_box_append(GTK_BOX(bottom_box), ui.bell_button);

    btn = gtk_button_new_with_label("+ New Workspace");
    gtk_widget_set_hexpand(btn, TRUE);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_add_workspace_clicked), NULL);
    gtk_box_append(GTK_BOX(bottom_box), btn);

    gtk_box_append(GTK_BOX(ui.sidebar_box), bottom_box);
}
