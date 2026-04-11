#pragma once

#include "app_ui.h"

typedef struct {
    AppUi ui;
    ghostty_app_t ghostty_app;
    float ghostty_default_font_size;
    GtkWindow *main_window;
    gboolean main_window_active;
    ghostty_config_t runtime_ghostty_config;
    GhosttyTerminal *terminal_search_target;
    gint64 terminal_search_total;
    gint64 terminal_search_selected;
} AppState;

AppState *app_state(void);

#define ui                         (app_state()->ui)
#define g_ghostty_app              (app_state()->ghostty_app)
#define g_ghostty_default_font_size (app_state()->ghostty_default_font_size)
#define g_main_window              (app_state()->main_window)
#define g_main_window_active       (app_state()->main_window_active)
#define g_runtime_ghostty_config   (app_state()->runtime_ghostty_config)
#define g_terminal_search_target   (app_state()->terminal_search_target)
#define g_terminal_search_total    (app_state()->terminal_search_total)
#define g_terminal_search_selected (app_state()->terminal_search_selected)
