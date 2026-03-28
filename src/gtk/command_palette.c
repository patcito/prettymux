/*
 * command_palette.c - Search overlay / command palette widget
 *
 * Shows a centred card with a search entry and a filtered list of all
 * workspaces, terminal tabs and browser tabs.  Arrow keys navigate,
 * Enter activates the selected item, Escape closes the palette.
 */

#include "command_palette.h"
#include "workspace.h"
#include "ghostty_terminal.h"
#include "browser_tab.h"
#include "theme.h"

#include <string.h>
#include <ctype.h>

/* ── Search-item kind ─────────────────────────────────────────── */

typedef enum {
    PALETTE_ITEM_WORKSPACE,
    PALETTE_ITEM_TERMINAL,
    PALETTE_ITEM_BROWSER,
} PaletteItemKind;

typedef struct {
    PaletteItemKind kind;
    char           *name;
    char           *detail;
    int             workspace_idx;
    int             pane_notebook_page;  /* which notebook page (terminal tab) */
    GtkNotebook    *pane_notebook;       /* the notebook containing the terminal */
    int             browser_tab_idx;
} PaletteItem;

/* ── Private structure ────────────────────────────────────────── */

struct _CommandPalette {
    GtkWidget    parent_instance;

    GtkWidget   *overlay_box;     /* semi-transparent full-size background  */
    GtkWidget   *card;            /* the centred search card                */
    GtkWidget   *search_entry;
    GtkWidget   *list_box;
    GtkWidget   *hint_label;

    /* External refs (not owned) */
    GtkWidget   *browser_notebook;
    GtkWidget   *terminal_stack;
    GtkWidget   *workspace_list;

    /* Gathered items */
    GPtrArray   *items;           /* of PaletteItem* */
};

G_DEFINE_FINAL_TYPE(CommandPalette, command_palette, GTK_TYPE_WIDGET)

/* ── Helpers ──────────────────────────────────────────────────── */

static PaletteItem *
palette_item_new(PaletteItemKind kind, const char *name, const char *detail)
{
    PaletteItem *item = g_new0(PaletteItem, 1);
    item->kind = kind;
    item->name = g_strdup(name ? name : "");
    item->detail = g_strdup(detail ? detail : "");
    item->workspace_idx = -1;
    item->pane_notebook_page = -1;
    item->pane_notebook = NULL;
    item->browser_tab_idx = -1;
    return item;
}

static void
palette_item_free(gpointer data)
{
    PaletteItem *item = data;
    g_free(item->name);
    g_free(item->detail);
    g_free(item);
}

static gboolean
str_contains_ci(const char *haystack, const char *needle)
{
    if (!haystack || !needle || !*needle)
        return TRUE;

    size_t hlen = strlen(haystack);
    size_t nlen = strlen(needle);
    if (nlen > hlen) return FALSE;

    for (size_t i = 0; i <= hlen - nlen; i++) {
        gboolean match = TRUE;
        for (size_t j = 0; j < nlen; j++) {
            if (tolower((unsigned char)haystack[i + j]) !=
                tolower((unsigned char)needle[j])) {
                match = FALSE;
                break;
            }
        }
        if (match) return TRUE;
    }
    return FALSE;
}

/* ── Gather all searchable items ──────────────────────────────── */

static void
palette_gather_items(CommandPalette *self)
{
    if (self->items)
        g_ptr_array_unref(self->items);
    self->items = g_ptr_array_new_with_free_func(palette_item_free);

    /* Workspaces + their terminal tabs */
    if (workspaces) {
        for (guint wi = 0; wi < workspaces->len; wi++) {
            Workspace *ws = g_ptr_array_index(workspaces, wi);

            PaletteItem *ws_item = palette_item_new(
                PALETTE_ITEM_WORKSPACE, ws->name, ws->cwd);
            ws_item->workspace_idx = (int)wi;
            g_ptr_array_add(self->items, ws_item);

            /* Iterate pane notebooks.  In the non-split case the
             * workspace container IS the notebook.  In the split case
             * the container is a GtkPaned tree; we walk them below.
             * For simplicity, iterate all terminals in ws->terminals
             * and find which notebook page they belong to. */

            /* Walk the workspace's pane_notebooks array if present,
             * otherwise fall back to the single notebook. */
            GPtrArray *notebooks = ws->pane_notebooks;
            if (notebooks) {
                for (guint ni = 0; ni < notebooks->len; ni++) {
                    GtkNotebook *nb = g_ptr_array_index(notebooks, ni);
                    int n_pages = gtk_notebook_get_n_pages(nb);
                    for (int ti = 0; ti < n_pages; ti++) {
                        GtkWidget *child = gtk_notebook_get_nth_page(nb, ti);
                        const char *title = NULL;
                        const char *cwd = NULL;
                        if (GHOSTTY_IS_TERMINAL(child)) {
                            title = ghostty_terminal_get_title(GHOSTTY_TERMINAL(child));
                            cwd = ghostty_terminal_get_cwd(GHOSTTY_TERMINAL(child));
                        }
                        char label[128];
                        snprintf(label, sizeof(label), "%s",
                                 (title && *title) ? title : "Terminal");

                        PaletteItem *t_item = palette_item_new(
                            PALETTE_ITEM_TERMINAL, label, cwd ? cwd : ws->cwd);
                        t_item->workspace_idx = (int)wi;
                        t_item->pane_notebook = nb;
                        t_item->pane_notebook_page = ti;
                        g_ptr_array_add(self->items, t_item);
                    }
                }
            }
        }
    }

    /* Browser tabs */
    if (self->browser_notebook) {
        int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(self->browser_notebook));
        for (int bi = 0; bi < n; bi++) {
            GtkWidget *child = gtk_notebook_get_nth_page(
                GTK_NOTEBOOK(self->browser_notebook), bi);
            const char *title = NULL;
            const char *url = NULL;
            if (BROWSER_IS_TAB(child)) {
                title = browser_tab_get_title(BROWSER_TAB(child));
                url = browser_tab_get_url(BROWSER_TAB(child));
            }
            PaletteItem *b_item = palette_item_new(
                PALETTE_ITEM_BROWSER,
                (title && *title) ? title : "Browser",
                url ? url : "");
            b_item->browser_tab_idx = bi;
            g_ptr_array_add(self->items, b_item);
        }
    }
}

/* ── Populate / filter the list box ───────────────────────────── */

static GtkWidget *
create_row_widget(PaletteItem *item)
{
    const char *icon;
    switch (item->kind) {
    case PALETTE_ITEM_WORKSPACE: icon = "\xe2\x96\xa3 "; break; /* "▣ " */
    case PALETTE_ITEM_TERMINAL:  icon = "\xe2\x96\xb8 "; break; /* "▸ " */
    case PALETTE_ITEM_BROWSER:   icon = "\xe2\x97\x89 "; break; /* "◉ " */
    }

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(hbox, 16);
    gtk_widget_set_margin_end(hbox, 16);
    gtk_widget_set_margin_top(hbox, 6);
    gtk_widget_set_margin_bottom(hbox, 6);

    char buf[256];
    snprintf(buf, sizeof(buf), "%s%s", icon, item->name);
    GtkWidget *name_label = gtk_label_new(buf);
    gtk_label_set_xalign(GTK_LABEL(name_label), 0);
    gtk_label_set_ellipsize(GTK_LABEL(name_label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(name_label, TRUE);
    gtk_box_append(GTK_BOX(hbox), name_label);

    if (item->detail && *item->detail) {
        GtkWidget *detail_label = gtk_label_new(item->detail);
        gtk_label_set_xalign(GTK_LABEL(detail_label), 1);
        gtk_label_set_ellipsize(GTK_LABEL(detail_label), PANGO_ELLIPSIZE_END);
        gtk_widget_set_size_request(detail_label, 180, -1);
        gtk_widget_add_css_class(detail_label, "dim-label");
        gtk_box_append(GTK_BOX(hbox), detail_label);
    }

    return hbox;
}

static void
palette_populate(CommandPalette *self, const char *query)
{
    /* Remove all existing rows */
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(self->list_box)) != NULL)
        gtk_list_box_remove(GTK_LIST_BOX(self->list_box), child);

    for (guint i = 0; i < self->items->len; i++) {
        PaletteItem *item = g_ptr_array_index(self->items, i);

        if (query && *query) {
            if (!str_contains_ci(item->name, query) &&
                !str_contains_ci(item->detail, query))
                continue;
        }

        GtkWidget *row_widget = create_row_widget(item);
        gtk_list_box_append(GTK_LIST_BOX(self->list_box), row_widget);

        /* Stash pointer on the row so we can retrieve it on activation */
        GtkListBoxRow *row = GTK_LIST_BOX_ROW(
            gtk_widget_get_parent(row_widget));
        if (row)
            g_object_set_data(G_OBJECT(row), "palette-item", item);
    }

    /* Select first row */
    GtkListBoxRow *first = gtk_list_box_get_row_at_index(
        GTK_LIST_BOX(self->list_box), 0);
    if (first)
        gtk_list_box_select_row(GTK_LIST_BOX(self->list_box), first);
}

/* ── Activation ───────────────────────────────────────────────── */

static void
palette_activate_item(CommandPalette *self, PaletteItem *item)
{
    if (!item) return;

    if (item->workspace_idx >= 0) {
        workspace_switch(item->workspace_idx,
                         self->terminal_stack,
                         self->workspace_list);
    }

    if (item->kind == PALETTE_ITEM_TERMINAL &&
        item->pane_notebook && item->pane_notebook_page >= 0) {
        gtk_notebook_set_current_page(item->pane_notebook,
                                      item->pane_notebook_page);
        GtkWidget *page = gtk_notebook_get_nth_page(
            item->pane_notebook, item->pane_notebook_page);
        if (page)
            gtk_widget_grab_focus(page);
    }

    if (item->kind == PALETTE_ITEM_BROWSER && item->browser_tab_idx >= 0) {
        gtk_notebook_set_current_page(
            GTK_NOTEBOOK(self->browser_notebook), item->browser_tab_idx);
        gtk_widget_set_visible(self->browser_notebook, TRUE);
    }

    /* Close the palette */
    gtk_widget_set_visible(GTK_WIDGET(self), FALSE);
}

/* ── Signal callbacks ─────────────────────────────────────────── */

static void
on_search_changed(GtkSearchEntry *entry, gpointer user_data)
{
    CommandPalette *self = COMMAND_PALETTE(user_data);
    const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));
    palette_populate(self, text);
}

static void
on_search_activate(GtkSearchEntry *entry, gpointer user_data)
{
    (void)entry;
    CommandPalette *self = COMMAND_PALETTE(user_data);
    GtkListBoxRow *row = gtk_list_box_get_selected_row(
        GTK_LIST_BOX(self->list_box));
    if (row) {
        PaletteItem *item = g_object_get_data(G_OBJECT(row), "palette-item");
        palette_activate_item(self, item);
    }
}

static void
on_row_activated(GtkListBox *list_box, GtkListBoxRow *row, gpointer user_data)
{
    (void)list_box;
    CommandPalette *self = COMMAND_PALETTE(user_data);
    PaletteItem *item = g_object_get_data(G_OBJECT(row), "palette-item");
    palette_activate_item(self, item);
}

static gboolean
on_key_pressed(GtkEventControllerKey *controller, guint keyval,
               guint keycode, GdkModifierType state, gpointer user_data)
{
    (void)controller; (void)keycode; (void)state;
    CommandPalette *self = COMMAND_PALETTE(user_data);

    if (keyval == GDK_KEY_Escape) {
        gtk_widget_set_visible(GTK_WIDGET(self), FALSE);
        return TRUE;
    }

    if (keyval == GDK_KEY_Down) {
        GtkListBoxRow *sel = gtk_list_box_get_selected_row(
            GTK_LIST_BOX(self->list_box));
        if (sel) {
            int idx = gtk_list_box_row_get_index(sel);
            GtkListBoxRow *next = gtk_list_box_get_row_at_index(
                GTK_LIST_BOX(self->list_box), idx + 1);
            if (next)
                gtk_list_box_select_row(GTK_LIST_BOX(self->list_box), next);
        }
        return TRUE;
    }

    if (keyval == GDK_KEY_Up) {
        GtkListBoxRow *sel = gtk_list_box_get_selected_row(
            GTK_LIST_BOX(self->list_box));
        if (sel) {
            int idx = gtk_list_box_row_get_index(sel);
            if (idx > 0) {
                GtkListBoxRow *prev = gtk_list_box_get_row_at_index(
                    GTK_LIST_BOX(self->list_box), idx - 1);
                if (prev)
                    gtk_list_box_select_row(GTK_LIST_BOX(self->list_box), prev);
            }
        }
        return TRUE;
    }

    return FALSE;
}

/* ── GObject lifecycle ────────────────────────────────────────── */

static void
command_palette_dispose(GObject *object)
{
    CommandPalette *self = COMMAND_PALETTE(object);

    if (self->overlay_box) {
        gtk_widget_unparent(self->overlay_box);
        self->overlay_box = NULL;
    }

    G_OBJECT_CLASS(command_palette_parent_class)->dispose(object);
}

static void
command_palette_finalize(GObject *object)
{
    CommandPalette *self = COMMAND_PALETTE(object);

    if (self->items) {
        g_ptr_array_unref(self->items);
        self->items = NULL;
    }

    G_OBJECT_CLASS(command_palette_parent_class)->finalize(object);
}

/* ── Class init ───────────────────────────────────────────────── */

static void
command_palette_class_init(CommandPaletteClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->dispose = command_palette_dispose;
    object_class->finalize = command_palette_finalize;

    gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
}

static void
command_palette_init(CommandPalette *self)
{
    self->items = NULL;
    self->browser_notebook = NULL;
    self->terminal_stack = NULL;
    self->workspace_list = NULL;

    /* ── Build widget tree ── */

    /* The overlay_box fills the entire palette widget and provides
     * a semi-transparent background. */
    self->overlay_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(self->overlay_box, "search-overlay");
    gtk_widget_set_hexpand(self->overlay_box, TRUE);
    gtk_widget_set_vexpand(self->overlay_box, TRUE);
    gtk_widget_set_halign(self->overlay_box, GTK_ALIGN_FILL);
    gtk_widget_set_valign(self->overlay_box, GTK_ALIGN_FILL);
    gtk_widget_set_parent(self->overlay_box, GTK_WIDGET(self));

    /* Centre-top card */
    self->card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(self->card, 520, -1);
    gtk_widget_set_halign(self->card, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(self->card, GTK_ALIGN_START);
    gtk_widget_set_margin_top(self->card, 80);
    gtk_box_append(GTK_BOX(self->overlay_box), self->card);

    /* Search entry */
    self->search_entry = gtk_search_entry_new();
    gtk_widget_set_margin_start(self->search_entry, 16);
    gtk_widget_set_margin_end(self->search_entry, 16);
    gtk_widget_set_margin_top(self->search_entry, 16);
    gtk_widget_set_margin_bottom(self->search_entry, 8);
    gtk_box_append(GTK_BOX(self->card), self->search_entry);

    /* Results list in a scrolled window */
    self->list_box = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(self->list_box),
                                    GTK_SELECTION_SINGLE);
    gtk_widget_add_css_class(self->list_box, "rich-list");

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), self->list_box);
    gtk_widget_set_size_request(scroll, -1, 300);
    gtk_box_append(GTK_BOX(self->card), scroll);

    /* Hint bar */
    self->hint_label = gtk_label_new(
        "\xe2\x86\x91\xe2\x86\x93 navigate    Enter select    Esc close");
    gtk_label_set_xalign(GTK_LABEL(self->hint_label), 0);
    gtk_widget_set_margin_start(self->hint_label, 16);
    gtk_widget_set_margin_end(self->hint_label, 16);
    gtk_widget_set_margin_top(self->hint_label, 4);
    gtk_widget_set_margin_bottom(self->hint_label, 8);
    gtk_widget_add_css_class(self->hint_label, "dim-label");
    gtk_box_append(GTK_BOX(self->card), self->hint_label);

    /* ── Signals ── */

    g_signal_connect(self->search_entry, "search-changed",
                     G_CALLBACK(on_search_changed), self);
    g_signal_connect(self->search_entry, "activate",
                     G_CALLBACK(on_search_activate), self);
    g_signal_connect(self->list_box, "row-activated",
                     G_CALLBACK(on_row_activated), self);

    /* Key controller on the search entry for arrow keys and Escape */
    GtkEventController *kc = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(kc, GTK_PHASE_CAPTURE);
    g_signal_connect(kc, "key-pressed", G_CALLBACK(on_key_pressed), self);
    gtk_widget_add_controller(self->search_entry, kc);
}

/* ── Public API ───────────────────────────────────────────────── */

GtkWidget *
command_palette_new(GtkWidget *browser_notebook,
                    GtkWidget *terminal_stack,
                    GtkWidget *workspace_list)
{
    CommandPalette *self = g_object_new(COMMAND_TYPE_PALETTE, NULL);
    self->browser_notebook = browser_notebook;
    self->terminal_stack = terminal_stack;
    self->workspace_list = workspace_list;
    return GTK_WIDGET(self);
}

void
command_palette_toggle(CommandPalette *self)
{
    g_return_if_fail(COMMAND_IS_PALETTE(self));

    gboolean vis = gtk_widget_get_visible(GTK_WIDGET(self));
    if (vis) {
        gtk_widget_set_visible(GTK_WIDGET(self), FALSE);
        return;
    }

    /* Refresh items and show */
    palette_gather_items(self);
    palette_populate(self, "");
    gtk_editable_set_text(GTK_EDITABLE(self->search_entry), "");
    gtk_widget_set_visible(GTK_WIDGET(self), TRUE);
    gtk_widget_grab_focus(self->search_entry);
}
