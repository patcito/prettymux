#pragma once

#include <glib.h>

#include "theme.h"

void app_settings_load(void);
void app_settings_save(void);

double app_settings_get_ghostty_font_size(void);
void app_settings_set_ghostty_font_size(double font_size);

const char *app_settings_get_ghostty_theme(void);
void app_settings_set_ghostty_theme(const char *theme_name);
const char *app_settings_default_ghostty_theme_for_prettymux_theme(const char *theme_name);

const char *app_settings_get_toast_position(void);
void app_settings_set_toast_position(const char *position);
gboolean app_settings_get_focus_on_hover(void);
void app_settings_set_focus_on_hover(gboolean enabled);

const Theme *app_settings_get_custom_theme(void);
void app_settings_set_custom_theme(const Theme *theme);

char *app_settings_ghostty_override_path(void);
void app_settings_write_ghostty_override(void);
