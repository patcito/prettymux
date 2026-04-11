#pragma once

#include <gtk/gtk.h>

#include "ghostty.h"
#include "ghostty_terminal.h"

typedef struct {
    GtkWidget *outer_paned;
    GtkWidget *sidebar_box;
    GtkWidget *workspace_search;
    GtkWidget *workspace_list;
    GtkWidget *main_paned;
    GtkWidget *terminal_box;
    GtkWidget *terminal_stack;
    GtkWidget *browser_notebook;
    GtkWidget *overlay;
    GtkWidget *command_palette;
    GtkWidget *bell_button;
    GtkWidget *toast_revealer;
    GtkWidget *toast_frame;
    GtkWidget *toast_label;
} AppUi;

void apply_runtime_settings(void *user_data);
void sync_ghostty_theme_to_prettymux_theme(void);
GhosttyTerminal *notebook_terminal_at(GtkNotebook *notebook, int page_num);
gboolean focus_within_terminal(GhosttyTerminal *term);
gboolean terminal_search_handle_key(guint keyval, GdkModifierType state);
void terminal_search_show(GhosttyTerminal *term, const char *needle);
