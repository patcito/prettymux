#include "theme.h"
#include <gtk/gtk.h>
#include <string.h>

int current_theme = 0;

static const Theme themes[] = {
    {"Dark",    "#1e1e2e", "#cdd6f4", "#181825", "#313244", "#6c7086", "#cba6f7",
                "#a6e3a1", "#f38ba8", "#f9e2af", "#89b4fa", "#fab387", "#45475a", "#585b70"},
    {"Light",   "#eff1f5", "#4c4f69", "#e6e9ef", "#ccd0da", "#8c8fa1", "#8839ef",
                "#40a02b", "#d20f39", "#df8e1d", "#1e66f5", "#fe640b", "#9ca0b0", "#acb0be"},
    {"Monokai", "#272822", "#f8f8f2", "#1e1f1c", "#3e3d32", "#75715e", "#ae81ff",
                "#a6e22e", "#f92672", "#e6db74", "#66d9ef", "#fd971f", "#49483e", "#75715e"},
};

static GtkCssProvider *css_provider = NULL;

const Theme *theme_get_current(void) {
    return &themes[current_theme];
}

void theme_cycle(void) {
    current_theme = (current_theme + 1) % THEME_COUNT;
    theme_apply();
}

void theme_set_by_name(const char *name) {
    for (int i = 0; i < THEME_COUNT; i++) {
        if (strcmp(themes[i].name, name) == 0) {
            current_theme = i;
            theme_apply();
            return;
        }
    }
}

void theme_apply(void) {
    const Theme *t = theme_get_current();
    char *css = g_strdup_printf(
        "window { background-color: %s; color: %s; }"
        ".sidebar { background-color: %s; }"
        ".sidebar-row { padding: 8px 12px; border-bottom: 1px solid %s; }"
        ".sidebar-row:selected { background-color: %s; }"
        ".browser-bar { background-color: %s; border-bottom: 1px solid %s; padding: 4px; }"
        ".browser-bar button { background: %s; color: %s; border: none; padding: 2px 8px;"
        "  border-radius: 4px; min-width: 24px; min-height: 24px; }"
        ".browser-bar entry { background: %s; color: %s; border: 1px solid %s;"
        "  border-radius: 4px; padding: 4px 8px; }"
        "notebook > header { background-color: %s; }"
        "notebook > header tab { padding: 4px 12px; color: %s; }"
        "notebook > header tab:checked { color: %s; border-bottom: 2px solid %s; }"
        ".search-overlay { background-color: alpha(%s, 0.95); border-radius: 8px;"
        "  padding: 16px; }"
        ".search-overlay entry { background: %s; color: %s; border: 1px solid %s;"
        "  border-radius: 4px; padding: 8px 12px; font-size: 14px; }"
        ".search-overlay list { background: transparent; }"
        ".search-overlay list row { padding: 8px 12px; color: %s; }"
        ".search-overlay list row:selected { background-color: %s; }"
        ".has-activity { color: %s; }"
        ".resize-overlay { background-color: alpha(%s, 0.92); color: %s;"
        "  border: 1px solid %s; border-radius: 6px; padding: 6px 14px;"
        "  font-size: 12px; font-family: monospace; }",
        t->bg, t->fg,
        t->surface,
        t->overlay,
        t->highlight,
        t->surface, t->overlay,
        t->muted, t->fg,
        t->bg, t->fg, t->overlay,
        t->surface,
        t->subtext,
        t->fg, t->accent,
        t->bg,
        t->surface, t->fg, t->overlay,
        t->fg,
        t->highlight,
        t->green,
        t->overlay, t->fg, t->muted
    );

    if (!css_provider) {
        css_provider = gtk_css_provider_new();
        gtk_style_context_add_provider_for_display(
            gdk_display_get_default(),
            GTK_STYLE_PROVIDER(css_provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    gtk_css_provider_load_from_string(css_provider, css);
    g_free(css);
}
