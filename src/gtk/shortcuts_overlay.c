/*
 * shortcuts_overlay.c - Premium keyboard shortcuts overlay
 *
 * Full-screen frosted overlay with categorized shortcuts in a clean,
 * minimal card design with pill-shaped key badges.
 */

#include "shortcuts_overlay.h"
#include "shortcuts.h"
#include "theme.h"

#include <stdio.h>
#include <string.h>

#define OVERLAY_NAME "shortcuts-overlay-box"

/* ── CSS injected once ─────────────────────────────────────────── */

static gboolean css_injected = FALSE;

typedef struct {
    GtkWidget *row;
    GtkWidget *label;
    GtkWidget *badges_box;
    GtkWidget *overlay_box;
    const char *action;
    gboolean editing;
} ShortcutRowData;

static void
inject_css(void)
{
    if (css_injected) return;
    css_injected = TRUE;

    const Theme *t = theme_get_current();
    char *css = g_strdup_printf(
        ".shortcuts-backdrop {"
        "  background-color: alpha(#000000, 0.65);"
        "}"
        ".shortcuts-card {"
        "  background-color: %s;"
        "  border-radius: 16px;"
        "  border: 1px solid alpha(%s, 0.3);"
        "  padding: 32px 40px;"
        "}"
        ".shortcuts-title {"
        "  font-size: 22px;"
        "  font-weight: 700;"
        "  letter-spacing: 0.5px;"
        "  color: %s;"
        "}"
        ".shortcuts-subtitle {"
        "  font-size: 12px;"
        "  color: %s;"
        "  letter-spacing: 1px;"
        "}"
        ".shortcuts-search {"
        "  background: alpha(%s, 0.5);"
        "  color: %s;"
        "  border: 1px solid alpha(%s, 0.2);"
        "  border-radius: 10px;"
        "  padding: 10px 14px;"
        "  font-size: 16px;"
        "  font-family: 'SF Mono', 'JetBrains Mono', monospace;"
        "  margin-bottom: 16px;"
        "}"
        ".shortcuts-search:focus {"
        "  border-color: %s;"
        "}"
        ".shortcuts-section {"
        "  font-size: 14px;"
        "  font-weight: 600;"
        "  letter-spacing: 1.5px;"
        "  color: %s;"
        "  margin-top: 20px;"
        "  margin-bottom: 8px;"
        "}"
        ".shortcut-row {"
        "  padding: 6px 0px;"
        "  border-radius: 8px;"
        "}"
        ".shortcut-row.editing {"
        "  background-color: alpha(%s, 0.14);"
        "}"
        ".shortcut-label {"
        "  font-size: 16px;"
        "  color: %s;"
        "}"
        ".key-badge {"
        "  background-color: alpha(%s, 0.6);"
        "  border: 1px solid alpha(%s, 0.15);"
        "  border-radius: 6px;"
        "  padding: 3px 10px;"
        "  font-size: 14px;"
        "  font-weight: 500;"
        "  font-family: 'SF Mono', 'JetBrains Mono', 'Fira Code', monospace;"
        "  color: %s;"
        "  min-height: 24px;"
        "}"
        ".shortcuts-divider {"
        "  background-color: alpha(%s, 0.2);"
        "  min-width: 1px;"
        "  margin: 0 16px;"
        "}"
        ".shortcuts-close {"
        "  background-color: %s;"
        "  color: %s;"
        "  border: none;"
        "  border-radius: 8px;"
        "  padding: 6px 20px;"
        "  font-size: 13px;"
        "  font-weight: 500;"
        "}"
        ".shortcuts-close:hover {"
        "  background-color: %s;"
        "}"
        ".shortcuts-reset {"
        "  background-color: alpha(%s, 0.75);"
        "  color: %s;"
        "  border: 1px solid alpha(%s, 0.2);"
        "  border-radius: 8px;"
        "  padding: 6px 20px;"
        "  font-size: 13px;"
        "  font-weight: 500;"
        "}"
        ".shortcuts-reset:hover {"
        "  background-color: alpha(%s, 0.95);"
        "}"
        ".shortcuts-esc-hint {"
        "  font-size: 11px;"
        "  color: %s;"
        "}",
        t->surface,            /* card bg */
        t->fg,                 /* card border */
        t->fg,                 /* title color */
        t->subtext,            /* subtitle */
        t->overlay, t->fg, t->fg, /* search: bg, text, border */
        t->accent,             /* search focus */
        t->accent,             /* section header */
        t->accent,             /* editing row bg */
        t->fg,                 /* shortcut label */
        t->overlay,            /* key badge bg */
        t->fg,                 /* key badge border */
        t->fg,                 /* key badge text */
        t->fg,                 /* divider */
        t->accent,             /* close btn bg */
        t->bg,                 /* close btn text */
        t->blue,               /* close btn hover */
        t->overlay,            /* reset btn bg */
        t->fg,                 /* reset btn text */
        t->fg,                 /* reset btn border */
        t->surface,            /* reset btn hover */
        t->subtext             /* esc hint */
    );

    GtkCssProvider *p = gtk_css_provider_new();
    gtk_css_provider_load_from_string(p, css);
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(p),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    g_free(css);
}

/* ── Format key combo into individual badges ──────────────────── */

static GtkWidget *
make_key_badges(guint keyval, GdkModifierType mods)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_halign(box, GTK_ALIGN_END);

    if (mods & GDK_CONTROL_MASK) {
        GtkWidget *b = gtk_label_new("Ctrl");
        gtk_widget_add_css_class(b, "key-badge");
        gtk_box_append(GTK_BOX(box), b);
    }
    if (mods & GDK_SHIFT_MASK) {
        GtkWidget *b = gtk_label_new("Shift");
        gtk_widget_add_css_class(b, "key-badge");
        gtk_box_append(GTK_BOX(box), b);
    }
    if (mods & GDK_ALT_MASK) {
        GtkWidget *b = gtk_label_new("Alt");
        gtk_widget_add_css_class(b, "key-badge");
        gtk_box_append(GTK_BOX(box), b);
    }
    if (mods & GDK_SUPER_MASK) {
        GtkWidget *b = gtk_label_new("Super");
        gtk_widget_add_css_class(b, "key-badge");
        gtk_box_append(GTK_BOX(box), b);
    }

    const char *name = gdk_keyval_name(keyval);
    if (name) {
        char pretty[32];
        /* Clean up key names */
        if (strcmp(name, "Return") == 0) snprintf(pretty, sizeof(pretty), "Enter");
        else if (strcmp(name, "bracketleft") == 0) snprintf(pretty, sizeof(pretty), "[");
        else if (strcmp(name, "bracketright") == 0) snprintf(pretty, sizeof(pretty), "]");
        else if (strcmp(name, "comma") == 0) snprintf(pretty, sizeof(pretty), ",");
        else {
            snprintf(pretty, sizeof(pretty), "%s", name);
            if (pretty[0] >= 'a' && pretty[0] <= 'z')
                pretty[0] = pretty[0] - 'a' + 'A';
        }
        GtkWidget *b = gtk_label_new(pretty);
        gtk_widget_add_css_class(b, "key-badge");
        gtk_box_append(GTK_BOX(box), b);
    }

    return box;
}

static gboolean
text_contains_case_insensitive(const char *haystack, const char *needle)
{
    gboolean found;
    char *haystack_folded;
    char *needle_folded;

    if (!haystack || !needle || !*needle)
        return TRUE;

    haystack_folded = g_utf8_casefold(haystack, -1);
    needle_folded = g_utf8_casefold(needle, -1);
    found = strstr(haystack_folded, needle_folded) != NULL;
    g_free(haystack_folded);
    g_free(needle_folded);
    return found;
}

static void
replace_badges_content(GtkWidget *badges_box, GtkWidget *content)
{
    GtkWidget *child;

    while ((child = gtk_widget_get_first_child(badges_box)) != NULL)
        gtk_box_remove(GTK_BOX(badges_box), child);
    if (content)
        gtk_box_append(GTK_BOX(badges_box), content);
}

/* ── Add shortcut row ─────────────────────────────────────────── */

static void
shortcut_row_refresh(ShortcutRowData *rd)
{
    const ShortcutDef *binding = shortcut_find_by_action(rd->action);
    if (!binding)
        return;

    gtk_widget_remove_css_class(rd->row, "editing");
    rd->editing = FALSE;
    if (g_object_get_data(G_OBJECT(rd->overlay_box), "editing-row") == rd)
        g_object_set_data(G_OBJECT(rd->overlay_box), "editing-row", NULL);
    replace_badges_content(rd->badges_box,
                           make_key_badges(binding->keyval, binding->mods));
}

static void
shortcut_row_show_message(ShortcutRowData *rd, const char *message)
{
    GtkWidget *msg = gtk_label_new(message);
    gtk_widget_add_css_class(msg, "key-badge");
    replace_badges_content(rd->badges_box, msg);
}

static gboolean
shortcut_is_modifier_only(guint keyval)
{
    switch (keyval) {
    case GDK_KEY_Shift_L:
    case GDK_KEY_Shift_R:
    case GDK_KEY_Control_L:
    case GDK_KEY_Control_R:
    case GDK_KEY_Alt_L:
    case GDK_KEY_Alt_R:
    case GDK_KEY_Meta_L:
    case GDK_KEY_Meta_R:
    case GDK_KEY_Super_L:
    case GDK_KEY_Super_R:
    case GDK_KEY_Hyper_L:
    case GDK_KEY_Hyper_R:
        return TRUE;
    default:
        return FALSE;
    }
}

static void
shortcut_row_begin_edit(ShortcutRowData *rd)
{
    ShortcutRowData *active =
        g_object_get_data(G_OBJECT(rd->overlay_box), "editing-row");
    if (active && active != rd)
        shortcut_row_refresh(active);

    rd->editing = TRUE;
    gtk_widget_add_css_class(rd->row, "editing");
    shortcut_row_show_message(rd, "Press shortcut");
    g_object_set_data(G_OBJECT(rd->overlay_box), "editing-row", rd);
    gtk_widget_grab_focus(rd->row);
}

static gboolean
on_shortcut_row_key_pressed(GtkEventControllerKey *ctrl, guint keyval,
                            guint keycode, GdkModifierType state,
                            gpointer user_data)
{
    ShortcutRowData *rd = user_data;
    const ShortcutDef *conflict = NULL;
    GdkModifierType mods;

    (void)ctrl;
    (void)keycode;

    if (!rd->editing)
        return FALSE;

    if (keyval == GDK_KEY_Escape) {
        shortcut_row_refresh(rd);
        g_object_set_data(G_OBJECT(rd->overlay_box), "editing-row", NULL);
        return TRUE;
    }

    if (shortcut_is_modifier_only(keyval))
        return TRUE;

    mods = state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK |
                    GDK_ALT_MASK | GDK_SUPER_MASK);

    if (!shortcut_set_binding(rd->action, keyval, mods, &conflict)) {
        const char *label = conflict && conflict->label
            ? conflict->label : "Already in use";
        char msg[128];
        snprintf(msg, sizeof(msg), "Used by %s", label);
        shortcut_row_show_message(rd, msg);
        return TRUE;
    }

    shortcut_row_refresh(rd);
    g_object_set_data(G_OBJECT(rd->overlay_box), "editing-row", NULL);
    return TRUE;
}

static void
on_shortcut_row_pressed(GtkGestureClick *gesture, int n_press,
                        double x, double y, gpointer user_data)
{
    ShortcutRowData *rd = user_data;
    (void)gesture;
    (void)x;
    (void)y;

    if (n_press == 2)
        shortcut_row_begin_edit(rd);
}

static void
shortcut_row_data_free(gpointer data)
{
    g_free(data);
}

static GtkWidget *
add_row(GtkWidget *column, GtkWidget *overlay_box,
        const char *action, const char *label_text, GtkWidget *badges)
{
    ShortcutRowData *rd;
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *badges_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(row, "shortcut-row");
    gtk_widget_set_focusable(row, TRUE);

    GtkWidget *lbl = gtk_label_new(label_text);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0);
    gtk_widget_set_hexpand(lbl, TRUE);
    gtk_widget_add_css_class(lbl, "shortcut-label");
    gtk_box_append(GTK_BOX(row), lbl);

    gtk_box_append(GTK_BOX(badges_box), badges);
    gtk_box_append(GTK_BOX(row), badges_box);

    rd = g_new0(ShortcutRowData, 1);
    rd->row = row;
    rd->label = lbl;
    rd->badges_box = badges_box;
    rd->overlay_box = overlay_box;
    rd->action = action;
    g_object_set_data_full(G_OBJECT(row), "shortcut-row-data", rd,
                           shortcut_row_data_free);

    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
    g_signal_connect(click, "pressed",
                     G_CALLBACK(on_shortcut_row_pressed), rd);
    gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(click));

    GtkEventController *key = gtk_event_controller_key_new();
    g_signal_connect(key, "key-pressed",
                     G_CALLBACK(on_shortcut_row_key_pressed), rd);
    gtk_widget_add_controller(row, key);

    gtk_box_append(GTK_BOX(column), row);
    return row;
}

static void
add_section(GtkWidget *column, const char *title)
{
    GtkWidget *lbl = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0);
    gtk_widget_add_css_class(lbl, "shortcuts-section");
    gtk_box_append(GTK_BOX(column), lbl);
}

/* ── Callbacks ─────────────────────────────────────────────────── */

static gboolean
on_overlay_key_pressed(GtkEventControllerKey *ctrl, guint keyval,
                       guint keycode, GdkModifierType state, gpointer user_data)
{
    (void)ctrl; (void)keycode;
    GtkWidget *overlay_box = GTK_WIDGET(user_data);

    if (g_object_get_data(G_OBJECT(overlay_box), "editing-row"))
        return FALSE;

    if (keyval == GDK_KEY_Escape) {
        GtkWidget *parent = gtk_widget_get_parent(overlay_box);
        if (GTK_IS_OVERLAY(parent))
            gtk_overlay_remove_overlay(GTK_OVERLAY(parent), overlay_box);
        return TRUE;
    }

    GdkModifierType masked = state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK |
                                       GDK_ALT_MASK | GDK_SUPER_MASK);
    if (gdk_keyval_to_lower(keyval) == GDK_KEY_k &&
        masked == (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) {
        GtkWidget *parent = gtk_widget_get_parent(overlay_box);
        if (GTK_IS_OVERLAY(parent))
            gtk_overlay_remove_overlay(GTK_OVERLAY(parent), overlay_box);
        return TRUE;
    }

    return FALSE;
}

static void
on_close_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    GtkWidget *overlay_box = GTK_WIDGET(user_data);
    GtkWidget *parent = gtk_widget_get_parent(overlay_box);
    if (GTK_IS_OVERLAY(parent))
        gtk_overlay_remove_overlay(GTK_OVERLAY(parent), overlay_box);
}

/* ── Find existing ─────────────────────────────────────────────── */

static GtkWidget *
find_existing(GtkOverlay *overlay)
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

/* ── Categorize shortcuts ──────────────────────────────────────── */

typedef struct {
    const char *section;
    const char *prefix;
} Category;

static const Category categories[] = {
    {"WORKSPACES",  "workspace."},
    {"TERMINAL",    "pane."},
    {"SPLITS",      "split."},
    {"BROWSER",     "browser."},
    {"TOOLS",       NULL},  /* catch-all */
};
#define N_CATEGORIES 5

static int
categorize(const char *action)
{
    if (g_str_has_prefix(action, "workspace.")) return 0;
    if (g_str_has_prefix(action, "pane.") || strcmp(action, "broadcast.toggle") == 0) return 1;
    if (g_str_has_prefix(action, "split.")) return 2;
    if (g_str_has_prefix(action, "browser.") || g_str_has_prefix(action, "devtools.") ||
        strcmp(action, "pip.toggle") == 0) return 3;
    return 4; /* tools: search, shortcuts, history, theme, notes, terminal, copy, paste */
}

/* ── Search filter for shortcuts ───────────────────────────────── */

static void
filter_column_rows(GtkWidget *col, const char *query)
{
    for (GtkWidget *child = gtk_widget_get_first_child(col);
         child; child = gtk_widget_get_next_sibling(child)) {
        /* Section headers: always show */
        if (gtk_widget_has_css_class(child, "shortcuts-section")) {
            gtk_widget_set_visible(child, TRUE);
            continue;
        }
        /* Shortcut rows: match label text against query */
        if (!gtk_widget_has_css_class(child, "shortcut-row")) continue;

        gboolean match = (!query || !*query);
        if (!match) {
            /* Check the label child for text match */
            for (GtkWidget *w = gtk_widget_get_first_child(child);
                 w; w = gtk_widget_get_next_sibling(w)) {
                if (GTK_IS_LABEL(w)) {
                    const char *text = gtk_label_get_text(GTK_LABEL(w));
                    if (text_contains_case_insensitive(text, query)) {
                        match = TRUE;
                        break;
                    }
                }
            }
        }
        gtk_widget_set_visible(child, match);
    }
}

static void
on_shortcuts_search_changed(GtkSearchEntry *entry, gpointer user_data)
{
    (void)user_data;
    const char *query = gtk_editable_get_text(GTK_EDITABLE(entry));
    GtkWidget *columns = g_object_get_data(G_OBJECT(entry), "columns");
    if (!columns) return;

    /* Iterate each column in the HBox */
    for (GtkWidget *col = gtk_widget_get_first_child(columns);
         col; col = gtk_widget_get_next_sibling(col)) {
        if (GTK_IS_BOX(col))
            filter_column_rows(col, query);
    }
}

static void
refresh_shortcut_rows(GtkWidget *columns)
{
    for (GtkWidget *col = gtk_widget_get_first_child(columns);
         col; col = gtk_widget_get_next_sibling(col)) {
        if (!GTK_IS_BOX(col))
            continue;
        for (GtkWidget *child = gtk_widget_get_first_child(col);
             child; child = gtk_widget_get_next_sibling(child)) {
            ShortcutRowData *rd =
                g_object_get_data(G_OBJECT(child), "shortcut-row-data");
            if (rd)
                shortcut_row_refresh(rd);
        }
    }
}

static void
on_reset_clicked(GtkButton *btn, gpointer user_data)
{
    GtkWidget *columns = GTK_WIDGET(user_data);
    (void)btn;
    shortcut_reset_all();
    refresh_shortcut_rows(columns);
}

/* ── Public API ────────────────────────────────────────────────── */

void
shortcuts_overlay_toggle(GtkOverlay *overlay)
{
    g_return_if_fail(GTK_IS_OVERLAY(overlay));
    shortcuts_init();

    GtkWidget *existing = find_existing(overlay);
    if (existing) {
        gtk_overlay_remove_overlay(overlay, existing);
        return;
    }

    inject_css();

    /* ── Backdrop (full screen, dark) ── */
    GtkWidget *backdrop = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_name(backdrop, OVERLAY_NAME);
    gtk_widget_add_css_class(backdrop, "shortcuts-backdrop");
    gtk_widget_set_hexpand(backdrop, TRUE);
    gtk_widget_set_vexpand(backdrop, TRUE);
    gtk_widget_set_halign(backdrop, GTK_ALIGN_FILL);
    gtk_widget_set_valign(backdrop, GTK_ALIGN_FILL);

    /* ── Card ── */
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(card, "shortcuts-card");
    gtk_widget_set_halign(card, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(card, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(card, 780, -1);
    gtk_box_append(GTK_BOX(backdrop), card);

    /* ── Header ── */
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_halign(header, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_bottom(header, 24);
    gtk_box_append(GTK_BOX(card), header);

    GtkWidget *title = gtk_label_new("Keyboard Shortcuts");
    gtk_widget_add_css_class(title, "shortcuts-title");
    gtk_box_append(GTK_BOX(header), title);

    GtkWidget *subtitle = gtk_label_new("PRETTYMUX");
    gtk_widget_add_css_class(subtitle, "shortcuts-subtitle");
    gtk_box_append(GTK_BOX(header), subtitle);

    /* ── Search entry ── */
    GtkWidget *search = gtk_search_entry_new();
    gtk_widget_add_css_class(search, "shortcuts-search");
    gtk_widget_set_margin_start(search, 8);
    gtk_widget_set_margin_end(search, 8);
    gtk_box_append(GTK_BOX(card), search);

    /* ── Three-column layout ── */
    GtkWidget *columns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(columns, TRUE);
    gtk_box_append(GTK_BOX(card), columns);

    GtkWidget *col1 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(col1, TRUE);
    gtk_box_append(GTK_BOX(columns), col1);

    GtkWidget *div1 = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    gtk_widget_add_css_class(div1, "shortcuts-divider");
    gtk_box_append(GTK_BOX(columns), div1);

    GtkWidget *col2 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(col2, TRUE);
    gtk_box_append(GTK_BOX(columns), col2);

    GtkWidget *div2 = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    gtk_widget_add_css_class(div2, "shortcuts-divider");
    gtk_box_append(GTK_BOX(columns), div2);

    GtkWidget *col3 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(col3, TRUE);
    gtk_box_append(GTK_BOX(columns), col3);

    /* Column targets: [col1=workspaces+terminal, col2=splits+browser, col3=tools] */
    GtkWidget *col_targets[] = { col1, col1, col2, col2, col3 };

    /* Add section headers */
    gboolean section_added[N_CATEGORIES] = {0};

    /* Populate from shortcuts table */
    for (int i = 0; i < shortcut_count(); i++) {
        const ShortcutDef *binding = shortcut_get_at(i);
        int cat;
        GtkWidget *col;

        if (!binding)
            continue;
        cat = categorize(binding->action);
        col = col_targets[cat];

        if (!section_added[cat]) {
            add_section(col, categories[cat].section);
            section_added[cat] = TRUE;
        }

        GtkWidget *badges = make_key_badges(binding->keyval, binding->mods);
        add_row(col, backdrop,
                binding->action,
                binding->label ? binding->label : binding->action,
                badges);
    }

    /* ── Footer ── */
    GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_top(footer, 24);
    gtk_widget_set_halign(footer, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(card), footer);

    GtkWidget *esc_hint = gtk_label_new("Press Esc or Ctrl+Shift+K to close");
    gtk_widget_add_css_class(esc_hint, "shortcuts-esc-hint");
    gtk_box_append(GTK_BOX(footer), esc_hint);

    GtkWidget *reset_btn = gtk_button_new_with_label("Reset to Factory");
    gtk_widget_add_css_class(reset_btn, "shortcuts-reset");
    g_signal_connect(reset_btn, "clicked", G_CALLBACK(on_reset_clicked), columns);
    gtk_box_append(GTK_BOX(footer), reset_btn);

    GtkWidget *close_btn = gtk_button_new_with_label("Done");
    gtk_widget_add_css_class(close_btn, "shortcuts-close");
    g_signal_connect(close_btn, "clicked", G_CALLBACK(on_close_clicked), backdrop);
    gtk_box_append(GTK_BOX(footer), close_btn);

    /* ── Key controller ── */
    GtkEventController *kc = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(kc, GTK_PHASE_CAPTURE);
    g_signal_connect(kc, "key-pressed", G_CALLBACK(on_overlay_key_pressed), backdrop);
    gtk_widget_add_controller(backdrop, kc);

    gtk_widget_set_focusable(backdrop, TRUE);
    gtk_overlay_add_overlay(overlay, backdrop);
    gtk_widget_set_visible(backdrop, TRUE);
    gtk_widget_grab_focus(search);

    /* Wire search filtering: on text change, show/hide shortcut-row widgets */
    g_object_set_data(G_OBJECT(search), "columns", columns);
    g_signal_connect(search, "search-changed",
                     G_CALLBACK(on_shortcuts_search_changed), NULL);
}
