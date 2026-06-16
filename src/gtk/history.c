#include "history.h"
#include <stdio.h>
#include <string.h>

/* Cap the in-memory history to the most recent N unique entries so a huge
 * ~/.bash_history doesn't load (and stay resident) in full. The overlay only
 * ever shows the most recent matches, so the older tail is never useful. */
#define HISTORY_MAX_ENTRIES 5000

struct _HistoryOverlay {
    GtkWidget *revealer;
    GtkWidget *entry;
    GtkWidget *list;
    GhosttyTerminal *target;
    GPtrArray *all_lines; // owned strings
};

static struct _HistoryOverlay hist_state = {0};

static void load_history(void) {
    if (hist_state.all_lines) {
        g_ptr_array_unref(hist_state.all_lines);
    }
    hist_state.all_lines = g_ptr_array_new_with_free_func(g_free);

    const char *histfile = g_getenv("HISTFILE");
    char *path = NULL;
    if (!histfile || !histfile[0]) {
        path = g_build_filename(g_get_home_dir(), ".bash_history", NULL);
        histfile = path;
    }

    FILE *fp = fopen(histfile, "r");
    g_free(path); /* fopen copied the name; safe to free now */
    if (!fp) return;

    /* Read every valid line in file order (oldest first). */
    GPtrArray *raw = g_ptr_array_new_with_free_func(g_free);
    char line[4096];
    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') line[len-1] = '\0';
        if (line[0] == '\0' || line[0] == '#') continue;
        g_ptr_array_add(raw, g_strdup(line));
    }
    fclose(fp);

    /* Walk newest -> oldest, keeping the newest occurrence of each unique
     * command, capped at HISTORY_MAX_ENTRIES. Dedup by newest (not first)
     * so a recently re-run command isn't dropped because an older copy
     * exists earlier in the file. `recent` borrows pointers from `raw`. */
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    GPtrArray *recent = g_ptr_array_new();
    for (int i = (int)raw->len - 1;
         i >= 0 && recent->len < HISTORY_MAX_ENTRIES; i--) {
        char *l = g_ptr_array_index(raw, i);
        if (!g_hash_table_contains(seen, l)) {
            g_hash_table_add(seen, l);
            g_ptr_array_add(recent, l);
        }
    }
    g_hash_table_unref(seen);

    /* Emit oldest -> newest (update_filter shows the tail first). */
    for (int i = (int)recent->len - 1; i >= 0; i--)
        g_ptr_array_add(hist_state.all_lines,
                        g_strdup(g_ptr_array_index(recent, i)));

    g_ptr_array_unref(recent);
    g_ptr_array_unref(raw);
}

static void update_filter(void) {
    const char *query = gtk_editable_get_text(GTK_EDITABLE(hist_state.entry));

    // Remove all children
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(hist_state.list)) != NULL)
        gtk_list_box_remove(GTK_LIST_BOX(hist_state.list), child);

    if (!hist_state.all_lines) return;

    int count = 0;
    // Show most recent first (reverse order)
    for (int i = (int)hist_state.all_lines->len - 1; i >= 0 && count < 50; i--) {
        const char *line = g_ptr_array_index(hist_state.all_lines, i);
        if (query[0] != '\0' && !strstr(line, query)) continue;

        GtkWidget *label = gtk_label_new(line);
        gtk_label_set_xalign(GTK_LABEL(label), 0);
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
        gtk_list_box_append(GTK_LIST_BOX(hist_state.list), label);
        count++;
    }

    // Select first row
    GtkListBoxRow *first = gtk_list_box_get_row_at_index(GTK_LIST_BOX(hist_state.list), 0);
    if (first) gtk_list_box_select_row(GTK_LIST_BOX(hist_state.list), first);
}

static void on_search_changed(GtkSearchEntry *entry, gpointer data) {
    (void)entry; (void)data;
    update_filter();
}

static void activate_selection(void) {
    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(hist_state.list));
    if (!row || !hist_state.target) return;

    GtkWidget *label = gtk_list_box_row_get_child(row);
    if (!GTK_IS_LABEL(label)) return;

    const char *text = gtk_label_get_text(GTK_LABEL(label));
    ghostty_surface_t surface = ghostty_terminal_get_surface(hist_state.target);
    if (surface && text)
        ghostty_surface_text(surface, text, strlen(text));

    history_overlay_hide(hist_state.revealer);
}

static void on_row_activated(GtkListBox *list, GtkListBoxRow *row, gpointer data) {
    (void)list; (void)row; (void)data;
    activate_selection();
}

static gboolean on_key_pressed(GtkEventControllerKey *ctrl, guint keyval,
                                guint keycode, GdkModifierType state, gpointer data)
{
    (void)ctrl; (void)keycode; (void)state; (void)data;
    if (keyval == GDK_KEY_Escape) {
        history_overlay_hide(hist_state.revealer);
        return TRUE;
    }
    if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
        activate_selection();
        return TRUE;
    }
    return FALSE;
}

GtkWidget *history_overlay_new(void) {
    hist_state.revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(hist_state.revealer),
                                     GTK_REVEALER_TRANSITION_TYPE_CROSSFADE);

    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(card, "search-overlay");
    gtk_widget_set_halign(card, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(card, GTK_ALIGN_START);
    gtk_widget_set_margin_top(card, 60);
    gtk_widget_set_size_request(card, 600, 400);

    GtkWidget *title = gtk_label_new("Command History");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(card), title);

    hist_state.entry = gtk_search_entry_new();
    gtk_box_append(GTK_BOX(card), hist_state.entry);
    g_signal_connect(hist_state.entry, "search-changed", G_CALLBACK(on_search_changed), NULL);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);

    hist_state.list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(hist_state.list), GTK_SELECTION_SINGLE);
    g_signal_connect(hist_state.list, "row-activated", G_CALLBACK(on_row_activated), NULL);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), hist_state.list);
    gtk_box_append(GTK_BOX(card), scroll);

    // Key controller
    GtkEventController *kc = gtk_event_controller_key_new();
    g_signal_connect(kc, "key-pressed", G_CALLBACK(on_key_pressed), NULL);
    gtk_widget_add_controller(card, kc);

    gtk_revealer_set_child(GTK_REVEALER(hist_state.revealer), card);
    return hist_state.revealer;
}

void history_overlay_show(GtkWidget *overlay, GhosttyTerminal *target) {
    (void)overlay;
    hist_state.target = target;
    load_history();
    gtk_editable_set_text(GTK_EDITABLE(hist_state.entry), "");
    update_filter();
    /* Make the overlay child targetable only while shown — otherwise it sits
     * over the whole window and swallows every click. */
    gtk_widget_set_visible(hist_state.revealer, TRUE);
    gtk_revealer_set_reveal_child(GTK_REVEALER(hist_state.revealer), TRUE);
    gtk_widget_grab_focus(hist_state.entry);
}

void history_overlay_hide(GtkWidget *overlay) {
    (void)overlay;
    gtk_revealer_set_reveal_child(GTK_REVEALER(hist_state.revealer), FALSE);
    gtk_widget_set_visible(hist_state.revealer, FALSE);
    hist_state.target = NULL;
    /* Release the loaded history so it isn't held resident while closed;
     * it's reloaded on next show. */
    if (hist_state.all_lines) {
        g_ptr_array_unref(hist_state.all_lines);
        hist_state.all_lines = NULL;
    }
}
