#pragma once

#include <glib.h>

#include "theme.h"
#include "workspace_layout.h"

void app_settings_load(void);
void app_settings_save(void);

double app_settings_get_ghostty_font_size(void);
void app_settings_set_ghostty_font_size(double font_size);

const char *app_settings_get_ghostty_theme(void);
void app_settings_set_ghostty_theme(const char *theme_name);
const char *app_settings_default_ghostty_theme_for_prettymux_theme(const char *theme_name);
void app_settings_ensure_ghostty_theme_default(const char *prettymux_theme_name);

const char *app_settings_get_toast_position(void);
void app_settings_set_toast_position(const char *position);
gboolean app_settings_get_focus_on_hover(void);
void app_settings_set_focus_on_hover(gboolean enabled);
gboolean app_settings_get_copy_on_select(void);
void app_settings_set_copy_on_select(gboolean enabled);
gboolean app_settings_get_copy_on_select_notified(void);
void app_settings_set_copy_on_select_notified(gboolean notified);
WorkspaceLayoutMode app_settings_get_default_layout_mode(void);
void app_settings_set_default_layout_mode(WorkspaceLayoutMode mode);

const char *app_settings_get_gtk_renderer_mode(void);
void app_settings_set_gtk_renderer_mode(const char *mode);
const char *app_settings_get_gtk_renderer_probe_result(void);
void app_settings_set_gtk_renderer_probe_result(const char *renderer);

int app_settings_get_tab_height(void);
void app_settings_set_tab_height(int height);

const Theme *app_settings_get_custom_theme(void);
void app_settings_set_custom_theme(const Theme *theme);

char *app_settings_ghostty_override_path(void);
void app_settings_write_ghostty_override(void);

/*
 * Ghostty theme discovery.
 *
 * app_settings_ghostty_theme_dirs: NULL-terminated list of directories that
 *   hold ghostty theme files (user config, $GHOSTTY_RESOURCES_DIR, and every
 *   system data dir). Caller frees with g_strfreev.
 * app_settings_resolve_ghostty_theme_path: absolute path to the theme file
 *   named `name`, or NULL if none is found. libghostty's own theme search
 *   dirs may be empty in a packaged install, so config is written with the
 *   resolved absolute path (which ghostty loads directly). Caller g_free()s.
 */
char **app_settings_ghostty_theme_dirs(void);
char  *app_settings_resolve_ghostty_theme_path(const char *name);

/*
 * app_settings_serialize_ghostty_theme: the value to write for `theme = ...`,
 *   with every component resolved to an absolute path. Handles ghostty's
 *   light/dark pair syntax ("dark:Foo,light:Bar") by resolving each side.
 *   Returns NULL when the theme cannot be resolved (caller should not write a
 *   theme at all rather than emit one ghostty will silently ignore).
 *   Caller g_free()s.
 */
char  *app_settings_serialize_ghostty_theme(const char *name);
