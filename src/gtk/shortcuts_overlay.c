/*
 * shortcuts_overlay.c - Keyboard shortcuts overlay
 *
 * Displays all keybindings in a two-column layout inside a card
 * overlaid on the main window.  Display-only (no rebinding).
 */

#include "shortcuts_overlay.h"
#include "shortcuts.h"
#include "theme.h"

#include <string.h>

/* Tag name used to find and toggle the overlay widget. */
#define OVERLAY_NAME "shortcuts-overlay-box"

/* ── Helpers ─────────────────────────────────────────────────── */

/* Build a human-readable string for a modifier+keyval combo. */
static void
format_shortcut(guint keyval, GdkModifierType mods, char *buf, size_t buflen)
{
    buf[0] = '\0';

    if (mods & GDK_CONTROL_MASK)
        g_strlcat(buf, "Ctrl+", buflen);
    if (mods & GDK_SHIFT_MASK)
        g_strlcat(buf, "Shift+", buflen);
    if (mods & GDK_ALT_MASK)
        g_strlcat(buf, "Alt+", buflen);
    if (mods & GDK_SUPER_MASK)
        g_strlcat(buf, "Super+", buflen);

    const char *name = gdk_keyval_name(keyval);
    if (name) {
        /* Capitalise first letter for display */
        char pretty[64];
        snprintf(pretty, sizeof(pretty), "%s", name);
        if (pretty[0] >= 'a' && pretty[0] <= 'z')
            pretty[0] = pretty[0] - 'a' + 'A';
        g_strlcat(buf, pretty, buflen);
    }
}

/* ── Callback: close on Escape or re-press of Ctrl+Shift+K ── */

static gboolean
on_overlay_key_pressed(GtkEventControllerKey *ctrl, guint keyval,
                       guint keycode, GdkModifierType state, gpointer user_data)
{
    (void)ctrl; (void)keycode;
    GtkWidget *overlay_box = GTK_WIDGET(user_data);

    if (keyval == GDK_KEY_Escape) {
        GtkWidget *parent = gtk_widget_get_parent(overlay_box);
        if (GTK_IS_OVERLAY(parent))
            gtk_overlay_remove_overlay(GTK_OVERLAY(parent), overlay_box);
        return TRUE;
    }

    /* Ctrl+Shift+K toggles off */
    GdkModifierType masked = state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK |
                                       GDK_ALT_MASK | GDK_SUPER_MASK);
    guint lower = gdk_keyval_to_lower(keyval);
    if (lower == GDK_KEY_k &&
        masked == (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) {
        GtkWidget *parent = gtk_widget_get_parent(overlay_box);
        if (GTK_IS_OVERLAY(parent))
            gtk_overlay_remove_overlay(GTK_OVERLAY(parent), overlay_box);
        return TRUE;
    }

    return FALSE;
}

static void
on_close_button_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    GtkWidget *overlay_box = GTK_WIDGET(user_data);
    GtkWidget *parent = gtk_widget_get_parent(overlay_box);
    if (GTK_IS_OVERLAY(parent))
        gtk_overlay_remove_overlay(GTK_OVERLAY(parent), overlay_box);
}

/* ── Add a single shortcut row to a column ───────────────────── */

static void
add_shortcut_row(GtkWidget *column, const char *label_text, const char *key_text)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(row, 0);
    gtk_widget_set_margin_end(row, 0);
    gtk_widget_set_margin_top(row, 3);
    gtk_widget_set_margin_bottom(row, 3);

    GtkWidget *lbl = gtk_label_new(label_text);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0);
    gtk_widget_set_hexpand(lbl, TRUE);
    gtk_box_append(GTK_BOX(row), lbl);

    GtkWidget *key_lbl = gtk_label_new(key_text);
    gtk_label_set_xalign(GTK_LABEL(key_lbl), 1);
    gtk_widget_add_css_class(key_lbl, "dim-label");
    gtk_widget_set_size_request(key_lbl, 140, -1);
    gtk_box_append(GTK_BOX(row), key_lbl);

    gtk_box_append(GTK_BOX(column), row);
}

static void
add_section_header(GtkWidget *column, const char *title)
{
    GtkWidget *lbl = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0);
    gtk_widget_add_css_class(lbl, "dim-label");
    gtk_widget_set_margin_top(lbl, 10);
    gtk_widget_set_margin_bottom(lbl, 2);
    gtk_box_append(GTK_BOX(column), lbl);
}

/* ── Find existing overlay ───────────────────────────────────── */

static GtkWidget *
find_existing_overlay(GtkOverlay *overlay)
{
    GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(overlay));
    while (child) {
        const char *name = gtk_widget_get_name(child);
        if (name && strcmp(name, OVERLAY_NAME) == 0)
            return child;
        child = gtk_widget_get_next_sibling(child);
    }
    return NULL;
}

/* ── Public API ──────────────────────────────────────────────── */

void
shortcuts_overlay_toggle(GtkOverlay *overlay)
{
    g_return_if_fail(GTK_IS_OVERLAY(overlay));

    /* If already showing, remove it */
    GtkWidget *existing = find_existing_overlay(overlay);
    if (existing) {
        gtk_overlay_remove_overlay(overlay, existing);
        return;
    }

    /* Build the overlay box (full-size, semi-transparent background) */
    GtkWidget *overlay_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_name(overlay_box, OVERLAY_NAME);
    gtk_widget_add_css_class(overlay_box, "search-overlay");
    gtk_widget_set_hexpand(overlay_box, TRUE);
    gtk_widget_set_vexpand(overlay_box, TRUE);
    gtk_widget_set_halign(overlay_box, GTK_ALIGN_FILL);
    gtk_widget_set_valign(overlay_box, GTK_ALIGN_FILL);

    /* Card (centred) */
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_size_request(card, 700, -1);
    gtk_widget_set_halign(card, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(card, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(card, 40);
    gtk_widget_set_margin_bottom(card, 40);
    gtk_widget_set_margin_start(card, 40);
    gtk_widget_set_margin_end(card, 40);
    gtk_box_append(GTK_BOX(overlay_box), card);

    /* Title */
    GtkWidget *title = gtk_label_new("Keyboard Shortcuts");
    gtk_widget_add_css_class(title, "title-2");
    gtk_label_set_xalign(GTK_LABEL(title), 0.5);
    gtk_widget_set_margin_bottom(title, 8);
    gtk_box_append(GTK_BOX(card), title);

    /* Two-column layout */
    GtkWidget *columns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 32);
    gtk_widget_set_hexpand(columns, TRUE);
    gtk_box_append(GTK_BOX(card), columns);

    GtkWidget *left_col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(left_col, TRUE);
    gtk_box_append(GTK_BOX(columns), left_col);

    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    gtk_box_append(GTK_BOX(columns), sep);

    GtkWidget *right_col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(right_col, TRUE);
    gtk_box_append(GTK_BOX(columns), right_col);

    /* Populate from the shortcuts table.
     * Split roughly in half between left and right columns. */
    int total = 0;
    for (int i = 0; default_shortcuts[i].action != NULL; i++)
        total++;

    int half = (total + 1) / 2;

    /* Left column: Workspaces, Panes, Power */
    add_section_header(left_col, "WORKSPACES & PANES");

    for (int i = 0; i < half && default_shortcuts[i].action != NULL; i++) {
        char key_str[128];
        format_shortcut(default_shortcuts[i].keyval,
                        default_shortcuts[i].mods,
                        key_str, sizeof(key_str));
        add_shortcut_row(left_col,
                         default_shortcuts[i].label ? default_shortcuts[i].label
                                                    : default_shortcuts[i].action,
                         key_str);
    }

    /* Right column: Browser, Window, etc. */
    add_section_header(right_col, "BROWSER & WINDOW");

    for (int i = half; default_shortcuts[i].action != NULL; i++) {
        char key_str[128];
        format_shortcut(default_shortcuts[i].keyval,
                        default_shortcuts[i].mods,
                        key_str, sizeof(key_str));
        add_shortcut_row(right_col,
                         default_shortcuts[i].label ? default_shortcuts[i].label
                                                    : default_shortcuts[i].action,
                         key_str);
    }

    /* Fixed shortcuts section */
    add_section_header(right_col, "FIXED SHORTCUTS");
    add_shortcut_row(right_col, "Switch workspace 1-9", "Ctrl+1-9");
    add_shortcut_row(right_col, "Focus pane", "Alt+Arrow");
    add_shortcut_row(right_col, "Cycle tabs", "Ctrl+Tab");
    add_shortcut_row(right_col, "Copy / Paste", "Ctrl+Shift+C/V");
    add_shortcut_row(right_col, "Fullscreen", "F11");

    /* Close button */
    GtkWidget *btn_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(btn_bar, GTK_ALIGN_END);
    gtk_widget_set_margin_top(btn_bar, 12);
    gtk_box_append(GTK_BOX(card), btn_bar);

    GtkWidget *close_btn = gtk_button_new_with_label("Close");
    g_signal_connect(close_btn, "clicked",
                     G_CALLBACK(on_close_button_clicked), overlay_box);
    gtk_box_append(GTK_BOX(btn_bar), close_btn);

    /* Key controller for Escape / Ctrl+Shift+K */
    GtkEventController *kc = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(kc, GTK_PHASE_CAPTURE);
    g_signal_connect(kc, "key-pressed",
                     G_CALLBACK(on_overlay_key_pressed), overlay_box);
    gtk_widget_add_controller(overlay_box, kc);

    /* Make the overlay focusable so it captures key events */
    gtk_widget_set_focusable(overlay_box, TRUE);

    /* Add to the GtkOverlay and show */
    gtk_overlay_add_overlay(overlay, overlay_box);
    gtk_widget_set_visible(overlay_box, TRUE);
    gtk_widget_grab_focus(overlay_box);
}
