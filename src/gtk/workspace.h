#pragma once

#include <gtk/gtk.h>
#include "ghostty.h"

typedef struct {
    char name[64];
    GtkWidget *container;     // Root widget for this workspace in the stack
    GtkWidget *notebook;      // Terminal tab notebook
    GPtrArray *terminals;     // Array of GhosttyTerminal widgets
    char cwd[512];
    char git_branch[128];
    char notification[256];
    gboolean broadcast;
} Workspace;

extern GPtrArray *workspaces;
extern int current_workspace;

Workspace *workspace_get_current(void);
void workspace_add(GtkWidget *terminal_stack, GtkWidget *workspace_list, ghostty_app_t app);
void workspace_remove(int index, GtkWidget *terminal_stack, GtkWidget *workspace_list);
void workspace_switch(int index, GtkWidget *terminal_stack, GtkWidget *workspace_list);
void workspace_add_terminal(Workspace *ws, ghostty_app_t app);
