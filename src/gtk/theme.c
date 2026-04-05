#include "theme.h"

#include <gtk/gtk.h>
#include <string.h>

enum {
    THEME_INDEX_DARK = 0,
    THEME_INDEX_LIGHT,
    THEME_INDEX_MONOKAI,
    THEME_INDEX_CUSTOM,
};

int current_theme = THEME_INDEX_DARK;

static Theme themes[THEME_COUNT] = {
    {"Dark",    "#1e1e2e", "#cdd6f4", "#181825", "#313244", "#6c7086", "#1f2430",
                "#cba6f7", "#a6e3a1", "#f38ba8", "#f9e2af", "#89b4fa",
                "#fab387", "#45475a", "#585b70"},
    {"Light",   "#eff1f5", "#4c4f69", "#e6e9ef", "#ccd0da", "#8c8fa1", "#ffffff",
                "#8839ef", "#40a02b", "#d20f39", "#df8e1d", "#1e66f5",
                "#fe640b", "#9ca0b0", "#acb0be"},
    {"Monokai", "#272822", "#f8f8f2", "#1e1f1c", "#3e3d32", "#75715e", "#1f201c",
                "#ae81ff", "#a6e22e", "#f92672", "#e6db74", "#66d9ef",
                "#fd971f", "#49483e", "#75715e"},
    {"Custom",  "#16181d", "#e7ecf3", "#1d222b", "#313847", "#98a3b8", "#1b2130",
                "#7c8cff", "#7ad8a4", "#ff7d96", "#f4c06b", "#6fb9ff",
                "#ffb38a", "#566179", "#8b74ff"},
};

static GtkCssProvider *css_provider = NULL;

static void
theme_init_default_custom_theme(void)
{
    static gboolean initialized = FALSE;
    Theme *custom = &themes[THEME_INDEX_CUSTOM];

    if (initialized)
        return;
    initialized = TRUE;

    custom->name = "Custom";
    custom->bg = g_strdup(custom->bg);
    custom->fg = g_strdup(custom->fg);
    custom->surface = g_strdup(custom->surface);
    custom->overlay = g_strdup(custom->overlay);
    custom->subtext = g_strdup(custom->subtext);
    custom->accent = g_strdup(custom->accent);
    custom->toast_bg = g_strdup(custom->toast_bg);
    custom->green = g_strdup(custom->green);
    custom->red = g_strdup(custom->red);
    custom->yellow = g_strdup(custom->yellow);
    custom->blue = g_strdup(custom->blue);
    custom->peach = g_strdup(custom->peach);
    custom->muted = g_strdup(custom->muted);
    custom->highlight = g_strdup(custom->highlight);
}

static void
theme_store_string(const char **slot, const char *value)
{
    char *copy = g_strdup(value ? value : "");
    g_free((gpointer)*slot);
    *(char **)slot = copy;
}

static void
theme_copy_custom(Theme *dest, const Theme *src)
{
    if (!dest || !src)
        return;

    dest->name = "Custom";
    theme_store_string(&dest->bg, src->bg);
    theme_store_string(&dest->fg, src->fg);
    theme_store_string(&dest->surface, src->surface);
    theme_store_string(&dest->overlay, src->overlay);
    theme_store_string(&dest->subtext, src->subtext);
    theme_store_string(&dest->accent, src->accent);
    theme_store_string(&dest->toast_bg, src->toast_bg);
    theme_store_string(&dest->green, src->green);
    theme_store_string(&dest->red, src->red);
    theme_store_string(&dest->yellow, src->yellow);
    theme_store_string(&dest->blue, src->blue);
    theme_store_string(&dest->peach, src->peach);
    theme_store_string(&dest->muted, src->muted);
    theme_store_string(&dest->highlight, src->highlight);
}

int
theme_count(void)
{
    theme_init_default_custom_theme();
    return THEME_COUNT;
}

const Theme *
theme_get_at(int index)
{
    theme_init_default_custom_theme();
    if (index < 0 || index >= THEME_COUNT)
        return NULL;
    return &themes[index];
}

const Theme *
theme_get_current(void)
{
    theme_init_default_custom_theme();
    return &themes[current_theme];
}

const char *
theme_get_default_tab_accent(const Theme *theme)
{
    if (!theme)
        return "#89b4fa";
    if (strcmp(theme->name, "Light") == 0)
        return "#1e66f5";
    if (strcmp(theme->name, "Monokai") == 0)
        return "#a6e22e";
    if (strcmp(theme->name, "Custom") == 0)
        return theme->accent;
    return "#89b4fa";
}

void
theme_cycle(void)
{
    current_theme = (current_theme + 1) % THEME_COUNT;
    theme_apply();
}

void
theme_set_by_name(const char *name)
{
    for (int i = 0; i < THEME_COUNT; i++) {
        if (strcmp(themes[i].name, name) == 0) {
            current_theme = i;
            theme_apply();
            return;
        }
    }
}

void
theme_set_custom(const Theme *theme)
{
    if (!theme)
        return;

    theme_init_default_custom_theme();
    theme_copy_custom(&themes[THEME_INDEX_CUSTOM], theme);

    if (current_theme == THEME_INDEX_CUSTOM)
        theme_apply();
}

const Theme *
theme_get_custom(void)
{
    theme_init_default_custom_theme();
    return &themes[THEME_INDEX_CUSTOM];
}

void
theme_apply(void)
{
    const Theme *t = theme_get_current();
    const char *tab_accent = theme_get_default_tab_accent(t);
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
        "paned > separator {"
        "  background-color: alpha(%s, 0.26);"
        "  min-width: 8px;"
        "  min-height: 8px;"
        "}"
        "paned > separator:hover {"
        "  background-color: alpha(%s, 0.42);"
        "}"
        "notebook > header { background-color: %s; }"
        "notebook > header tab {"
        "  padding: 8px 12px;"
        "  min-height: 42px;"
        "  color: %s;"
        "  border-bottom: 2px solid transparent;"
        "  box-shadow: none;"
        "  outline: none;"
        "  background-image: none;"
        "}"
        "notebook > header tab:checked {"
        "  color: %s;"
        "  border-bottom: 2px solid %s;"
        "  box-shadow: none;"
        "  outline: none;"
        "  background-image: none;"
        "}"
        ".tab-art-box {"
        "  background: transparent;"
        "  border-radius: 7px;"
        "  padding: 1px;"
        "}"
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
        "  font-size: 12px; font-family: monospace; }"
        ".prettymux-toast {"
        "  background-color: %s;"
        "  color: %s;"
        "  border: 1px solid %s;"
        "  box-shadow: 0 10px 30px alpha(black, 0.24);"
        "  opacity: 1;"
        "}"
        ".prettymux-toast button {"
        "  color: %s;"
        "}"
        ".terminal-status { font-size: 20px; font-family: monospace;"
        "  color: %s; padding: 1px 4px; }"
        ".tab-blink {"
        "  border-left: 3px solid %s;"
        "  border-radius: 2px;"
        "  transition: border-color 0.3s ease;"
        "}",
        t->bg, t->fg,
        t->surface,
        t->overlay,
        t->highlight,
        t->surface, t->overlay,
        t->muted, t->fg,
        t->bg, t->fg, t->overlay,
        t->highlight,
        t->accent,
        t->surface,
        t->subtext,
        t->fg, tab_accent,
        t->bg,
        t->surface, t->fg, t->overlay,
        t->fg,
        t->highlight,
        t->green,
        t->overlay, t->fg, t->muted,
        t->toast_bg, t->fg, t->muted, t->fg,
        t->subtext,
        tab_accent
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
