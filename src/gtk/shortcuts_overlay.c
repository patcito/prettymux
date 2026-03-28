/*
 * shortcuts_overlay.c - Premium keyboard shortcuts overlay
 *
 * Full-screen frosted overlay with categorized shortcuts in a clean,
 * minimal card design with pill-shaped key badges.
 */

#include "shortcuts_overlay.h"
#include "shortcuts.h"
#include "theme.h"

#include <string.h>

#define OVERLAY_NAME "shortcuts-overlay-box"

/* ── CSS injected once ─────────────────────────────────────────── */

static gboolean css_injected = FALSE;

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
        ".shortcuts-section {"
        "  font-size: 11px;"
        "  font-weight: 600;"
        "  letter-spacing: 1.5px;"
        "  color: %s;"
        "  margin-top: 16px;"
        "  margin-bottom: 6px;"
        "}"
        ".shortcut-row {"
        "  padding: 5px 0px;"
        "}"
        ".shortcut-label {"
        "  font-size: 13px;"
        "  color: %s;"
        "}"
        ".key-badge {"
        "  background-color: alpha(%s, 0.6);"
        "  border: 1px solid alpha(%s, 0.15);"
        "  border-radius: 6px;"
        "  padding: 2px 8px;"
        "  font-size: 11px;"
        "  font-weight: 500;"
        "  font-family: 'SF Mono', 'JetBrains Mono', 'Fira Code', monospace;"
        "  color: %s;"
        "  min-height: 20px;"
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
        ".shortcuts-esc-hint {"
        "  font-size: 11px;"
        "  color: %s;"
        "}",
        t->surface,            /* card bg */
        t->fg,                 /* card border */
        t->fg,                 /* title color */
        t->subtext,            /* subtitle */
        t->accent,             /* section header */
        t->fg,                 /* shortcut label */
        t->overlay,            /* key badge bg */
        t->fg,                 /* key badge border */
        t->fg,                 /* key badge text */
        t->fg,                 /* divider */
        t->accent,             /* close btn bg */
        t->bg,                 /* close btn text */
        t->blue,               /* close btn hover */
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

/* ── Make a fixed-text badge row (for non-table shortcuts) ────── */

static GtkWidget *
make_text_badges(const char *keys)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_halign(box, GTK_ALIGN_END);

    /* Split by + and create individual badges */
    char *copy = g_strdup(keys);
    char *token = strtok(copy, "+");
    while (token) {
        /* Trim whitespace */
        while (*token == ' ') token++;
        char *end = token + strlen(token) - 1;
        while (end > token && *end == ' ') *end-- = '\0';

        if (*token) {
            GtkWidget *b = gtk_label_new(token);
            gtk_widget_add_css_class(b, "key-badge");
            gtk_box_append(GTK_BOX(box), b);
        }
        token = strtok(NULL, "+");
    }
    g_free(copy);
    return box;
}

/* ── Add shortcut row ─────────────────────────────────────────── */

static void
add_row(GtkWidget *column, const char *label_text, GtkWidget *badges)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(row, "shortcut-row");

    GtkWidget *lbl = gtk_label_new(label_text);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0);
    gtk_widget_set_hexpand(lbl, TRUE);
    gtk_widget_add_css_class(lbl, "shortcut-label");
    gtk_box_append(GTK_BOX(row), lbl);

    gtk_box_append(GTK_BOX(row), badges);
    gtk_box_append(GTK_BOX(column), row);
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

/* ── Public API ────────────────────────────────────────────────── */

void
shortcuts_overlay_toggle(GtkOverlay *overlay)
{
    g_return_if_fail(GTK_IS_OVERLAY(overlay));

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

    /* Column targets: [col1=workspaces+terminal, col2=splits+browser, col3=tools+fixed] */
    GtkWidget *col_targets[] = { col1, col1, col2, col2, col3 };

    /* Add section headers */
    gboolean section_added[N_CATEGORIES] = {0};

    /* Populate from shortcuts table */
    for (int i = 0; default_shortcuts[i].action != NULL; i++) {
        int cat = categorize(default_shortcuts[i].action);
        GtkWidget *col = col_targets[cat];

        if (!section_added[cat]) {
            add_section(col, categories[cat].section);
            section_added[cat] = TRUE;
        }

        GtkWidget *badges = make_key_badges(default_shortcuts[i].keyval,
                                             default_shortcuts[i].mods);
        add_row(col,
                default_shortcuts[i].label ? default_shortcuts[i].label
                                           : default_shortcuts[i].action,
                badges);
    }

    /* Fixed shortcuts in col3 */
    add_section(col3, "NAVIGATION");
    add_row(col3, "Switch workspace 1-9", make_text_badges("Ctrl + 1-9"));
    add_row(col3, "Navigate panes", make_text_badges("Alt + Arrow"));
    add_row(col3, "Fullscreen", make_text_badges("F11"));
    add_row(col3, "Close browser tab", make_text_badges("Ctrl + W"));
    add_row(col3, "New browser tab", make_text_badges("Ctrl + T"));

    /* ── Footer ── */
    GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_top(footer, 24);
    gtk_widget_set_halign(footer, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(card), footer);

    GtkWidget *esc_hint = gtk_label_new("Press Esc or Ctrl+Shift+K to close");
    gtk_widget_add_css_class(esc_hint, "shortcuts-esc-hint");
    gtk_box_append(GTK_BOX(footer), esc_hint);

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
    gtk_widget_grab_focus(backdrop);
}
