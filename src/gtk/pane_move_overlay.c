/*
 * pane_move_overlay.c - Searchable pane picker for moving the current tab
 */

#include "pane_move_overlay.h"

#include "ghostty_terminal.h"
#include "project_icon_cache.h"
#include "theme.h"
#include "workspace.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define OVERLAY_NAME "pane-move-overlay-box"

typedef struct {
    int workspace_idx;
    int pane_idx;
    int tab_idx;
    gboolean valid;
} SourceTabLocation;

typedef struct {
    char *title;
    char *detail;
    char *pane_label;
    char *icon_path;
    int workspace_idx;
    int pane_idx;
} PaneMoveItem;

typedef struct {
    GtkOverlay *overlay;
    GtkWidget *backdrop;
    GtkWidget *card;
    GtkWidget *search_entry;
    GtkWidget *list_box;
    GtkWidget *empty_label;
    GtkWidget *terminal_stack;
    GtkWidget *workspace_list;
    GPtrArray *items;
    SourceTabLocation source;
} PaneMoveOverlayState;

static gboolean css_injected = FALSE;

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

static const char *
terminal_icon_path(GtkWidget *terminal)
{
    const char *cwd;
    const char *icon_path;

    if (!terminal || !GHOSTTY_IS_TERMINAL(terminal))
        return NULL;

    icon_path = g_object_get_data(G_OBJECT(terminal), "project-icon-path");
    if (icon_path && icon_path[0] &&
        g_file_test(icon_path, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_REGULAR))
        return icon_path;

    cwd = ghostty_terminal_get_cwd(GHOSTTY_TERMINAL(terminal));
    if (!cwd || !cwd[0])
        return NULL;

    return project_icon_cache_lookup_for_path(cwd);
}

static void
pane_move_item_free(gpointer data)
{
    PaneMoveItem *item = data;

    g_free(item->title);
    g_free(item->detail);
    g_free(item->pane_label);
    g_free(item->icon_path);
    g_free(item);
}

static void
pane_move_state_free(gpointer data)
{
    PaneMoveOverlayState *state = data;

    if (state->items)
        g_ptr_array_unref(state->items);
    g_free(state);
}

static gboolean
str_contains_ci(const char *haystack, const char *needle)
{
    if (!haystack || !needle || !*needle)
        return TRUE;

    size_t hlen = strlen(haystack);
    size_t nlen = strlen(needle);
    if (nlen > hlen)
        return FALSE;

    for (size_t i = 0; i <= hlen - nlen; i++) {
        gboolean match = TRUE;
        for (size_t j = 0; j < nlen; j++) {
            if (tolower((unsigned char)haystack[i + j]) !=
                tolower((unsigned char)needle[j])) {
                match = FALSE;
                break;
            }
        }
        if (match)
            return TRUE;
    }

    return FALSE;
}

static void
inject_css(void)
{
    GtkCssProvider *provider;
    const Theme *t;
    char *css;

    if (css_injected)
        return;
    css_injected = TRUE;

    t = theme_get_current();
    css = g_strdup_printf(
        ".pane-move-backdrop {"
        "  background-color: alpha(#03060a, 0.72);"
        "}"
        ".pane-move-card {"
        "  background-color: %s;"
        "  border-radius: 22px;"
        "  border: 1px solid alpha(%s, 0.18);"
        "  padding: 28px 30px 22px 30px;"
        "}"
        ".pane-move-kicker {"
        "  font-size: 11px;"
        "  letter-spacing: 1.6px;"
        "  font-weight: 700;"
        "  color: %s;"
        "}"
        ".pane-move-title {"
        "  font-size: 26px;"
        "  font-weight: 700;"
        "  color: %s;"
        "}"
        ".pane-move-subtitle {"
        "  font-size: 13px;"
        "  color: %s;"
        "}"
        ".pane-move-search {"
        "  background: alpha(%s, 0.52);"
        "  color: %s;"
        "  border: 1px solid alpha(%s, 0.14);"
        "  border-radius: 12px;"
        "  padding: 12px 14px;"
        "  margin-top: 18px;"
        "  margin-bottom: 16px;"
        "  font-size: 15px;"
        "}"
        ".pane-move-search:focus {"
        "  border-color: %s;"
        "}"
        ".pane-move-list {"
        "  background: transparent;"
        "}"
        ".pane-move-list row {"
        "  background: transparent;"
        "  border-radius: 14px;"
        "  margin: 0 0 10px 0;"
        "  padding: 0;"
        "}"
        ".pane-move-list row:selected {"
        "  background-color: alpha(%s, 0.12);"
        "}"
        ".pane-move-row {"
        "  padding: 14px 16px;"
        "}"
        ".pane-move-favicon-box {"
        "  background: #ffffff;"
        "  border-radius: 7px;"
        "  padding: 1px;"
        "}"
        ".pane-move-badge {"
        "  min-width: 46px;"
        "  min-height: 28px;"
        "  padding: 4px 10px;"
        "  border-radius: 999px;"
        "  background-color: alpha(%s, 0.82);"
        "  color: %s;"
        "  font-size: 11px;"
        "  font-weight: 700;"
        "  letter-spacing: 0.9px;"
        "}"
        ".pane-move-name {"
        "  font-size: 17px;"
        "  font-weight: 650;"
        "  color: %s;"
        "}"
        ".pane-move-detail {"
        "  font-size: 13px;"
        "  color: %s;"
        "}"
        ".pane-move-pill {"
        "  border-radius: 999px;"
        "  padding: 5px 12px;"
        "  background-color: alpha(%s, 0.72);"
        "  color: %s;"
        "  border: 1px solid alpha(%s, 0.14);"
        "  font-size: 12px;"
        "  font-weight: 600;"
        "}"
        ".pane-move-empty {"
        "  font-size: 14px;"
        "  color: %s;"
        "  padding: 24px 8px 10px 8px;"
        "}"
        ".pane-move-footer {"
        "  margin-top: 14px;"
        "}"
        ".pane-move-hint {"
        "  font-size: 11px;"
        "  color: %s;"
        "}"
        ".pane-move-cancel {"
        "  background-color: alpha(%s, 0.75);"
        "  color: %s;"
        "  border: 1px solid alpha(%s, 0.2);"
        "  border-radius: 8px;"
        "  padding: 6px 18px;"
        "  font-size: 13px;"
        "  font-weight: 500;"
        "}"
        ".pane-move-cancel:hover {"
        "  background-color: alpha(%s, 0.95);"
        "}",
        t->surface,
        t->fg,
        t->accent,
        t->fg,
        t->subtext,
        t->overlay,
        t->fg,
        t->fg,
        t->accent,
        t->accent,
        t->accent,
        t->bg,
        t->fg,
        t->subtext,
        t->overlay,
        t->fg,
        t->fg,
        t->subtext,
        t->subtext,
        t->overlay,
        t->fg,
        t->fg,
        t->surface
    );

    provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider, css, -1);
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    g_object_unref(provider);
    g_free(css);
}

static GtkWidget *
find_existing(GtkOverlay *overlay)
{
    GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(overlay));

    while (child) {
        if (g_strcmp0(gtk_widget_get_name(child), OVERLAY_NAME) == 0)
            return child;
        child = gtk_widget_get_next_sibling(child);
    }

    return NULL;
}

static int
workspace_pane_index(Workspace *ws, GtkNotebook *notebook)
{
    if (!ws || !ws->pane_notebooks || !GTK_IS_NOTEBOOK(notebook))
        return -1;

    for (guint i = 0; i < ws->pane_notebooks->len; i++) {
        if (g_ptr_array_index(ws->pane_notebooks, i) == notebook)
            return (int)i;
    }

    return -1;
}

static gboolean
locate_current_tab(SourceTabLocation *loc)
{
    Workspace *ws;
    GtkNotebook *notebook;
    int pane_idx;
    int tab_idx;

    if (!loc)
        return FALSE;

    memset(loc, 0, sizeof(*loc));
    loc->workspace_idx = -1;
    loc->pane_idx = -1;
    loc->tab_idx = -1;
    loc->valid = FALSE;

    ws = workspace_get_current();
    if (!ws)
        return FALSE;

    notebook = workspace_get_focused_pane(ws);
    if (!GTK_IS_NOTEBOOK(notebook))
        return FALSE;

    pane_idx = workspace_pane_index(ws, notebook);
    tab_idx = gtk_notebook_get_current_page(notebook);
    if (pane_idx < 0 || tab_idx < 0)
        return FALSE;

    loc->workspace_idx = current_workspace;
    loc->pane_idx = pane_idx;
    loc->tab_idx = tab_idx;
    loc->valid = TRUE;
    return TRUE;
}

static const char *
terminal_summary(GtkWidget *terminal, Workspace *ws)
{
    const char *cwd;
    const char *title;

    if (!terminal || !GHOSTTY_IS_TERMINAL(terminal))
        return (ws && ws->cwd[0]) ? ws->cwd : "";

    cwd = ghostty_terminal_get_cwd(GHOSTTY_TERMINAL(terminal));
    if (cwd && cwd[0])
        return cwd;

    title = ghostty_terminal_get_title(GHOSTTY_TERMINAL(terminal));
    if (title && title[0])
        return title;

    return (ws && ws->cwd[0]) ? ws->cwd : "";
}

static void
gather_items(PaneMoveOverlayState *state)
{
    if (state->items)
        g_ptr_array_unref(state->items);
    state->items = g_ptr_array_new_with_free_func(pane_move_item_free);

    if (!state->source.valid || !workspaces)
        return;

    for (guint wi = 0; wi < workspaces->len; wi++) {
        Workspace *ws = g_ptr_array_index(workspaces, wi);

        if (!ws || !ws->pane_notebooks)
            continue;

        for (guint pi = 0; pi < ws->pane_notebooks->len; pi++) {
            GtkNotebook *nb = g_ptr_array_index(ws->pane_notebooks, pi);
            GtkWidget *terminal;
            const char *summary;
            PaneMoveItem *item;
            int n_pages;
            int active_page;

            if (!GTK_IS_NOTEBOOK(nb))
                continue;
            if ((int)wi == state->source.workspace_idx &&
                (int)pi == state->source.pane_idx)
                continue;

            n_pages = gtk_notebook_get_n_pages(nb);
            active_page = gtk_notebook_get_current_page(nb);
            if (active_page < 0 && n_pages > 0)
                active_page = 0;

            terminal = active_page >= 0
                ? page_linked_terminal(gtk_notebook_get_nth_page(nb, active_page))
                : NULL;
            summary = terminal_summary(terminal, ws);

            item = g_new0(PaneMoveItem, 1);
            item->workspace_idx = (int)wi;
            item->pane_idx = (int)pi;
            item->title = g_strdup((ws->name && ws->name[0]) ? ws->name
                                                             : "Workspace");
            item->pane_label = g_strdup_printf("Pane %d", (int)pi + 1);
            item->detail = g_strdup_printf("%s  •  %d tab%s%s%s",
                                           item->pane_label,
                                           n_pages,
                                           n_pages == 1 ? "" : "s",
                                           (summary && summary[0]) ? "  •  " : "",
                                           (summary && summary[0]) ? summary : "");
            item->icon_path = g_strdup(terminal_icon_path(terminal));
            g_ptr_array_add(state->items, item);
        }
    }
}

static GtkWidget *
create_row_widget(PaneMoveItem *item)
{
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    GtkWidget *icon_box = NULL;
    GtkWidget *icon = NULL;
    GtkWidget *badge = gtk_label_new("MOVE");
    GtkWidget *text_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    GtkWidget *name = gtk_label_new(item->title);
    GtkWidget *detail = gtk_label_new(item->detail);
    GtkWidget *pill = gtk_label_new(item->pane_label);

    gtk_widget_add_css_class(hbox, "pane-move-row");
    gtk_widget_add_css_class(badge, "pane-move-badge");
    gtk_widget_add_css_class(name, "pane-move-name");
    gtk_widget_add_css_class(detail, "pane-move-detail");
    gtk_widget_add_css_class(pill, "pane-move-pill");

    gtk_label_set_xalign(GTK_LABEL(name), 0);
    gtk_label_set_xalign(GTK_LABEL(detail), 0);
    gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
    gtk_label_set_ellipsize(GTK_LABEL(detail), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(text_box, TRUE);
    gtk_widget_set_halign(pill, GTK_ALIGN_END);
    gtk_widget_set_valign(pill, GTK_ALIGN_CENTER);

    if (item->icon_path && item->icon_path[0]) {
        icon_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        icon = gtk_image_new_from_file(item->icon_path);
        gtk_widget_add_css_class(icon_box, "pane-move-favicon-box");
        gtk_image_set_pixel_size(GTK_IMAGE(icon), 16);
        gtk_widget_set_size_request(icon, 18, 18);
        gtk_widget_set_size_request(icon_box, 20, 20);
        gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(icon_box, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(icon_box), icon);
        gtk_box_append(GTK_BOX(hbox), icon_box);
    }

    gtk_box_append(GTK_BOX(text_box), name);
    gtk_box_append(GTK_BOX(text_box), detail);

    gtk_box_append(GTK_BOX(hbox), badge);
    gtk_box_append(GTK_BOX(hbox), text_box);
    gtk_box_append(GTK_BOX(hbox), pill);
    return hbox;
}

static void
populate_rows(PaneMoveOverlayState *state, const char *query)
{
    GtkWidget *child;
    gboolean found = FALSE;

    while ((child = gtk_widget_get_first_child(state->list_box)) != NULL)
        gtk_list_box_remove(GTK_LIST_BOX(state->list_box), child);

    for (guint i = 0; i < state->items->len; i++) {
        PaneMoveItem *item = g_ptr_array_index(state->items, i);
        GtkWidget *row_widget;
        GtkListBoxRow *row;

        if (query && *query &&
            !str_contains_ci(item->title, query) &&
            !str_contains_ci(item->detail, query))
            continue;

        row_widget = create_row_widget(item);
        gtk_list_box_append(GTK_LIST_BOX(state->list_box), row_widget);
        row = GTK_LIST_BOX_ROW(gtk_widget_get_parent(row_widget));
        if (row)
            g_object_set_data(G_OBJECT(row), "pane-move-item", item);
        found = TRUE;
    }

    if (found) {
        GtkListBoxRow *first = gtk_list_box_get_row_at_index(
            GTK_LIST_BOX(state->list_box), 0);
        gtk_widget_set_visible(state->list_box, TRUE);
        gtk_widget_set_visible(state->empty_label, FALSE);
        if (first)
            gtk_list_box_select_row(GTK_LIST_BOX(state->list_box), first);
    } else {
        if (state->items->len == 0) {
            gtk_label_set_text(GTK_LABEL(state->empty_label),
                "No other panes available yet. Split a pane or create another workspace.");
        } else {
            gtk_label_set_text(GTK_LABEL(state->empty_label),
                "No panes match that search.");
        }
        gtk_widget_set_visible(state->list_box, FALSE);
        gtk_widget_set_visible(state->empty_label, TRUE);
    }
}

static void
close_overlay(PaneMoveOverlayState *state)
{
    if (!state || !state->overlay || !state->backdrop)
        return;

    gtk_overlay_remove_overlay(state->overlay, state->backdrop);
}

static void
activate_row(PaneMoveOverlayState *state, GtkListBoxRow *row)
{
    PaneMoveItem *item;

    if (!state || !row || !state->source.valid)
        return;

    item = g_object_get_data(G_OBJECT(row), "pane-move-item");
    if (!item)
        return;

    if (workspace_move_tab(state->source.workspace_idx,
                           state->source.pane_idx,
                           state->source.tab_idx,
                           item->workspace_idx,
                           item->pane_idx)) {
        workspace_switch(item->workspace_idx,
                         state->terminal_stack,
                         state->workspace_list);
        close_overlay(state);
    }
}

static void
on_search_changed(GtkSearchEntry *entry, gpointer user_data)
{
    PaneMoveOverlayState *state = user_data;
    const char *query = gtk_editable_get_text(GTK_EDITABLE(entry));

    populate_rows(state, query);
}

static void
on_search_activate(GtkSearchEntry *entry, gpointer user_data)
{
    PaneMoveOverlayState *state = user_data;
    GtkListBoxRow *row;
    (void)entry;

    row = gtk_list_box_get_selected_row(GTK_LIST_BOX(state->list_box));
    activate_row(state, row);
}

static void
on_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
    (void)box;
    activate_row(user_data, row);
}

static void
select_relative_row(PaneMoveOverlayState *state, int delta)
{
    GtkListBox *box;
    GtkListBoxRow *row;
    int index;

    if (!state || !gtk_widget_get_visible(state->list_box))
        return;

    box = GTK_LIST_BOX(state->list_box);
    row = gtk_list_box_get_selected_row(box);
    if (!row)
        row = gtk_list_box_get_row_at_index(box, 0);
    if (!row)
        return;

    index = gtk_list_box_row_get_index(row) + delta;
    if (index < 0)
        index = 0;

    row = gtk_list_box_get_row_at_index(box, index);
    if (row)
        gtk_list_box_select_row(box, row);
}

static gboolean
on_key_pressed(GtkEventControllerKey *ctrl, guint keyval,
               guint keycode, GdkModifierType state, gpointer user_data)
{
    PaneMoveOverlayState *overlay_state = user_data;
    (void)ctrl;
    (void)keycode;
    (void)state;

    switch (keyval) {
    case GDK_KEY_Escape:
        close_overlay(overlay_state);
        return TRUE;
    case GDK_KEY_Up:
        select_relative_row(overlay_state, -1);
        return TRUE;
    case GDK_KEY_Down:
        select_relative_row(overlay_state, 1);
        return TRUE;
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter: {
        GtkListBoxRow *row = gtk_list_box_get_selected_row(
            GTK_LIST_BOX(overlay_state->list_box));
        activate_row(overlay_state, row);
        return TRUE;
    }
    default:
        return FALSE;
    }
}

static void
on_cancel_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    close_overlay(user_data);
}

void
pane_move_overlay_toggle(GtkOverlay *overlay,
                         GtkWidget *terminal_stack,
                         GtkWidget *workspace_list)
{
    PaneMoveOverlayState *state;
    GtkWidget *existing;
    GtkWidget *header;
    GtkWidget *title;
    GtkWidget *subtitle;
    GtkWidget *kicker;
    GtkWidget *search;
    GtkWidget *scroll;
    GtkWidget *footer;
    GtkWidget *hint;
    GtkWidget *cancel;
    GtkEventController *kc;

    g_return_if_fail(GTK_IS_OVERLAY(overlay));

    existing = find_existing(overlay);
    if (existing) {
        gtk_overlay_remove_overlay(overlay, existing);
        return;
    }

    inject_css();

    state = g_new0(PaneMoveOverlayState, 1);
    state->overlay = overlay;
    state->terminal_stack = terminal_stack;
    state->workspace_list = workspace_list;
    locate_current_tab(&state->source);

    state->backdrop = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_name(state->backdrop, OVERLAY_NAME);
    gtk_widget_add_css_class(state->backdrop, "pane-move-backdrop");
    gtk_widget_set_hexpand(state->backdrop, TRUE);
    gtk_widget_set_vexpand(state->backdrop, TRUE);
    gtk_widget_set_halign(state->backdrop, GTK_ALIGN_FILL);
    gtk_widget_set_valign(state->backdrop, GTK_ALIGN_FILL);
    g_object_set_data_full(G_OBJECT(state->backdrop), "pane-move-state",
                           state, pane_move_state_free);

    state->card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(state->card, "pane-move-card");
    gtk_widget_set_halign(state->card, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(state->card, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(state->card, 760, -1);
    gtk_box_append(GTK_BOX(state->backdrop), state->card);

    header = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_bottom(header, 4);
    gtk_box_append(GTK_BOX(state->card), header);

    kicker = gtk_label_new("TAB FLOW");
    gtk_widget_add_css_class(kicker, "pane-move-kicker");
    gtk_label_set_xalign(GTK_LABEL(kicker), 0);
    gtk_box_append(GTK_BOX(header), kicker);

    title = gtk_label_new("Move Current Tab");
    gtk_widget_add_css_class(title, "pane-move-title");
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    gtk_box_append(GTK_BOX(header), title);

    subtitle = gtk_label_new("Choose any destination pane across your workspaces.");
    gtk_widget_add_css_class(subtitle, "pane-move-subtitle");
    gtk_label_set_xalign(GTK_LABEL(subtitle), 0);
    gtk_box_append(GTK_BOX(header), subtitle);

    search = gtk_search_entry_new();
    state->search_entry = search;
    gtk_widget_add_css_class(search, "pane-move-search");
    gtk_editable_set_text(GTK_EDITABLE(search), "");
    gtk_box_append(GTK_BOX(state->card), search);

    state->list_box = gtk_list_box_new();
    gtk_widget_add_css_class(state->list_box, "pane-move-list");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(state->list_box),
                                    GTK_SELECTION_SINGLE);
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(state->list_box),
                                              TRUE);

    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), state->list_box);
    gtk_widget_set_size_request(scroll, -1, 340);
    gtk_box_append(GTK_BOX(state->card), scroll);

    state->empty_label = gtk_label_new("");
    gtk_widget_add_css_class(state->empty_label, "pane-move-empty");
    gtk_label_set_wrap(GTK_LABEL(state->empty_label), TRUE);
    gtk_label_set_xalign(GTK_LABEL(state->empty_label), 0.5);
    gtk_widget_set_visible(state->empty_label, FALSE);
    gtk_box_append(GTK_BOX(state->card), state->empty_label);

    footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(footer, "pane-move-footer");
    gtk_widget_set_halign(footer, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(state->card), footer);

    hint = gtk_label_new("Type to filter  •  ↑↓ navigate  •  Enter moves  •  Esc closes");
    gtk_widget_add_css_class(hint, "pane-move-hint");
    gtk_box_append(GTK_BOX(footer), hint);

    cancel = gtk_button_new_with_label("Cancel");
    gtk_widget_add_css_class(cancel, "pane-move-cancel");
    g_signal_connect(cancel, "clicked", G_CALLBACK(on_cancel_clicked), state);
    gtk_box_append(GTK_BOX(footer), cancel);

    gather_items(state);
    populate_rows(state, "");

    g_signal_connect(search, "search-changed",
                     G_CALLBACK(on_search_changed), state);
    g_signal_connect(search, "activate",
                     G_CALLBACK(on_search_activate), state);
    g_signal_connect(state->list_box, "row-activated",
                     G_CALLBACK(on_row_activated), state);

    kc = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(kc, GTK_PHASE_CAPTURE);
    g_signal_connect(kc, "key-pressed", G_CALLBACK(on_key_pressed), state);
    gtk_widget_add_controller(state->backdrop, kc);

    gtk_widget_set_focusable(state->backdrop, TRUE);
    gtk_overlay_add_overlay(overlay, state->backdrop);
    gtk_widget_set_visible(state->backdrop, TRUE);
    gtk_widget_grab_focus(search);
}
