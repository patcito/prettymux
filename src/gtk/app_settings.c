#include "app_settings.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

typedef struct {
    gboolean loaded;
    double ghostty_font_size;
    char *ghostty_theme;
    char *toast_position;
    gboolean focus_on_hover;
    gboolean open_links_in_browser;
    Theme custom_theme;
} AppSettingsState;

static AppSettingsState app_settings = {
    .loaded = FALSE,
    .ghostty_font_size = 0.0,
    .ghostty_theme = NULL,
    .toast_position = NULL,
    .focus_on_hover = TRUE,
    .open_links_in_browser = TRUE,
    .custom_theme = {
        .name = "Custom",
        .bg = "#16181d",
        .fg = "#e7ecf3",
        .surface = "#1d222b",
        .overlay = "#313847",
        .subtext = "#98a3b8",
        .accent = "#7c8cff",
        .toast_bg = "#1b2130",
        .green = "#7ad8a4",
        .red = "#ff7d96",
        .yellow = "#f4c06b",
        .blue = "#6fb9ff",
        .peach = "#ffb38a",
        .muted = "#566179",
        .highlight = "#8b74ff",
    },
};

static void
app_settings_init_default_custom_theme(void)
{
    static gboolean initialized = FALSE;

    if (initialized)
        return;
    initialized = TRUE;

    app_settings.custom_theme.name = "Custom";
    app_settings.custom_theme.bg = g_strdup(app_settings.custom_theme.bg);
    app_settings.custom_theme.fg = g_strdup(app_settings.custom_theme.fg);
    app_settings.custom_theme.surface = g_strdup(app_settings.custom_theme.surface);
    app_settings.custom_theme.overlay = g_strdup(app_settings.custom_theme.overlay);
    app_settings.custom_theme.subtext = g_strdup(app_settings.custom_theme.subtext);
    app_settings.custom_theme.accent = g_strdup(app_settings.custom_theme.accent);
    app_settings.custom_theme.toast_bg = g_strdup(app_settings.custom_theme.toast_bg);
    app_settings.custom_theme.green = g_strdup(app_settings.custom_theme.green);
    app_settings.custom_theme.red = g_strdup(app_settings.custom_theme.red);
    app_settings.custom_theme.yellow = g_strdup(app_settings.custom_theme.yellow);
    app_settings.custom_theme.blue = g_strdup(app_settings.custom_theme.blue);
    app_settings.custom_theme.peach = g_strdup(app_settings.custom_theme.peach);
    app_settings.custom_theme.muted = g_strdup(app_settings.custom_theme.muted);
    app_settings.custom_theme.highlight = g_strdup(app_settings.custom_theme.highlight);
}

static char *
app_settings_path(void)
{
    return g_build_filename(g_get_home_dir(), ".config", "prettymux",
                            "settings.ini", NULL);
}

char *
app_settings_ghostty_override_path(void)
{
    return g_build_filename(g_get_home_dir(), ".config", "prettymux",
                            "ghostty-overrides.conf", NULL);
}

static void
app_settings_store_theme_string(const char **slot, const char *value)
{
    char *copy = g_strdup(value ? value : "");
    g_free((gpointer)*slot);
    *(char **)slot = copy;
}

static void
app_settings_copy_theme(Theme *dest, const Theme *src)
{
    if (!dest || !src)
        return;

    dest->name = "Custom";
    app_settings_store_theme_string(&dest->bg, src->bg);
    app_settings_store_theme_string(&dest->fg, src->fg);
    app_settings_store_theme_string(&dest->surface, src->surface);
    app_settings_store_theme_string(&dest->overlay, src->overlay);
    app_settings_store_theme_string(&dest->subtext, src->subtext);
    app_settings_store_theme_string(&dest->accent, src->accent);
    app_settings_store_theme_string(&dest->toast_bg, src->toast_bg);
    app_settings_store_theme_string(&dest->green, src->green);
    app_settings_store_theme_string(&dest->red, src->red);
    app_settings_store_theme_string(&dest->yellow, src->yellow);
    app_settings_store_theme_string(&dest->blue, src->blue);
    app_settings_store_theme_string(&dest->peach, src->peach);
    app_settings_store_theme_string(&dest->muted, src->muted);
    app_settings_store_theme_string(&dest->highlight, src->highlight);
}

static void
app_settings_load_theme_colors(GKeyFile *kf, const char *group, Theme *theme)
{
    const struct {
        const char *key;
        const char **slot;
    } fields[] = {
        {"bg", &theme->bg},
        {"fg", &theme->fg},
        {"surface", &theme->surface},
        {"overlay", &theme->overlay},
        {"subtext", &theme->subtext},
        {"accent", &theme->accent},
        {"toast_bg", &theme->toast_bg},
        {"green", &theme->green},
        {"red", &theme->red},
        {"yellow", &theme->yellow},
        {"blue", &theme->blue},
        {"peach", &theme->peach},
        {"muted", &theme->muted},
        {"highlight", &theme->highlight},
    };

    for (guint i = 0; i < G_N_ELEMENTS(fields); i++) {
        if (g_key_file_has_key(kf, group, fields[i].key, NULL)) {
            char *value = g_key_file_get_string(kf, group, fields[i].key, NULL);
            if (value && value[0])
                app_settings_store_theme_string(fields[i].slot, value);
            g_free(value);
        }
    }
}

void
app_settings_write_ghostty_override(void)
{
    GString *contents;
    char *path;
    char *dir;

    app_settings_load();

    contents = g_string_new("");
    if (app_settings.ghostty_font_size > 0.0)
        g_string_append_printf(contents, "font-size = %.1f\n",
                               app_settings.ghostty_font_size);
    if (app_settings.ghostty_theme && app_settings.ghostty_theme[0])
        g_string_append_printf(contents, "theme = %s\n",
                               app_settings.ghostty_theme);

    path = app_settings_ghostty_override_path();
    dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);

    if (contents->len > 0)
        g_file_set_contents(path, contents->str, (gssize)contents->len, NULL);
    else
        g_remove(path);

    g_free(dir);
    g_free(path);
    g_string_free(contents, TRUE);
}

void
app_settings_load(void)
{
    GKeyFile *kf;
    char *path;

    if (app_settings.loaded)
        return;
    app_settings.loaded = TRUE;
    app_settings_init_default_custom_theme();

    kf = g_key_file_new();
    path = app_settings_path();
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        if (g_key_file_has_key(kf, "ghostty", "font_size", NULL))
            app_settings.ghostty_font_size =
                g_key_file_get_double(kf, "ghostty", "font_size", NULL);
        if (g_key_file_has_key(kf, "ghostty", "theme", NULL))
            app_settings.ghostty_theme =
                g_key_file_get_string(kf, "ghostty", "theme", NULL);
        if (g_key_file_has_key(kf, "ui", "toast_position", NULL))
            app_settings.toast_position =
                g_key_file_get_string(kf, "ui", "toast_position", NULL);
        if (g_key_file_has_key(kf, "ui", "focus_on_hover", NULL))
            app_settings.focus_on_hover =
                g_key_file_get_boolean(kf, "ui", "focus_on_hover", NULL);
        if (g_key_file_has_key(kf, "ui", "open_links_in_browser", NULL))
            app_settings.open_links_in_browser =
                g_key_file_get_boolean(kf, "ui", "open_links_in_browser", NULL);
        app_settings_load_theme_colors(kf, "custom_theme",
                                       &app_settings.custom_theme);
    }

    if (!app_settings.ghostty_theme)
        app_settings.ghostty_theme = g_strdup("");
    if (!app_settings.toast_position || !app_settings.toast_position[0]) {
        g_free(app_settings.toast_position);
        app_settings.toast_position = g_strdup("top");
    }

    g_free(path);
    g_key_file_unref(kf);
}

void
app_settings_save(void)
{
    GKeyFile *kf;
    char *path;
    char *dir;
    char *data;
    gsize len = 0;

    app_settings_load();

    kf = g_key_file_new();
    g_key_file_set_double(kf, "ghostty", "font_size",
                          app_settings.ghostty_font_size);
    g_key_file_set_string(kf, "ghostty", "theme",
                          app_settings.ghostty_theme ?
                              app_settings.ghostty_theme : "");
    g_key_file_set_string(kf, "ui", "toast_position",
                          app_settings.toast_position &&
                          app_settings.toast_position[0]
                              ? app_settings.toast_position
                              : "top");
    g_key_file_set_boolean(kf, "ui", "focus_on_hover",
                           app_settings.focus_on_hover);
    g_key_file_set_boolean(kf, "ui", "open_links_in_browser",
                           app_settings.open_links_in_browser);

    g_key_file_set_string(kf, "custom_theme", "bg", app_settings.custom_theme.bg);
    g_key_file_set_string(kf, "custom_theme", "fg", app_settings.custom_theme.fg);
    g_key_file_set_string(kf, "custom_theme", "surface", app_settings.custom_theme.surface);
    g_key_file_set_string(kf, "custom_theme", "overlay", app_settings.custom_theme.overlay);
    g_key_file_set_string(kf, "custom_theme", "subtext", app_settings.custom_theme.subtext);
    g_key_file_set_string(kf, "custom_theme", "accent", app_settings.custom_theme.accent);
    g_key_file_set_string(kf, "custom_theme", "toast_bg", app_settings.custom_theme.toast_bg);
    g_key_file_set_string(kf, "custom_theme", "green", app_settings.custom_theme.green);
    g_key_file_set_string(kf, "custom_theme", "red", app_settings.custom_theme.red);
    g_key_file_set_string(kf, "custom_theme", "yellow", app_settings.custom_theme.yellow);
    g_key_file_set_string(kf, "custom_theme", "blue", app_settings.custom_theme.blue);
    g_key_file_set_string(kf, "custom_theme", "peach", app_settings.custom_theme.peach);
    g_key_file_set_string(kf, "custom_theme", "muted", app_settings.custom_theme.muted);
    g_key_file_set_string(kf, "custom_theme", "highlight", app_settings.custom_theme.highlight);

    path = app_settings_path();
    dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    data = g_key_file_to_data(kf, &len, NULL);
    g_file_set_contents(path, data, (gssize)len, NULL);

    g_free(data);
    g_free(dir);
    g_free(path);
    g_key_file_unref(kf);

    app_settings_write_ghostty_override();
}

double
app_settings_get_ghostty_font_size(void)
{
    app_settings_load();
    return app_settings.ghostty_font_size;
}

void
app_settings_set_ghostty_font_size(double font_size)
{
    app_settings_load();
    app_settings.ghostty_font_size = font_size > 0.0 ? font_size : 0.0;
}

const char *
app_settings_get_ghostty_theme(void)
{
    app_settings_load();
    return app_settings.ghostty_theme ? app_settings.ghostty_theme : "";
}

void
app_settings_set_ghostty_theme(const char *theme_name)
{
    app_settings_load();
    g_free(app_settings.ghostty_theme);
    app_settings.ghostty_theme = g_strdup(theme_name ? theme_name : "");
}

const char *
app_settings_get_toast_position(void)
{
    app_settings_load();
    return app_settings.toast_position ? app_settings.toast_position : "top";
}

void
app_settings_set_toast_position(const char *position)
{
    app_settings_load();
    g_free(app_settings.toast_position);
    if (g_strcmp0(position, "bottom") == 0)
        app_settings.toast_position = g_strdup("bottom");
    else
        app_settings.toast_position = g_strdup("top");
}

gboolean
app_settings_get_focus_on_hover(void)
{
    app_settings_load();
    return app_settings.focus_on_hover;
}

void
app_settings_set_focus_on_hover(gboolean enabled)
{
    app_settings_load();
    app_settings.focus_on_hover = enabled != FALSE;
}

gboolean
app_settings_get_open_links_in_browser(void)
{
    app_settings_load();
    return app_settings.open_links_in_browser;
}

void
app_settings_set_open_links_in_browser(gboolean enabled)
{
    app_settings_load();
    app_settings.open_links_in_browser = enabled != FALSE;
}

const char *
app_settings_default_ghostty_theme_for_prettymux_theme(const char *theme_name)
{
    return g_strcmp0(theme_name, "Light") == 0
        ? "Light Owl"
        : "Catppuccin Frappe";
}

const Theme *
app_settings_get_custom_theme(void)
{
    app_settings_load();
    return &app_settings.custom_theme;
}

void
app_settings_set_custom_theme(const Theme *theme)
{
    app_settings_load();
    if (!theme)
        return;

    app_settings_copy_theme(&app_settings.custom_theme, theme);
}
