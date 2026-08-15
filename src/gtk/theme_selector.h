/*
 * theme_selector.h - Per-scope ghostty color-theme picker.
 *
 * prettymux can override the ghostty terminal color theme independently for
 * a single tab, a whole pane (notebook), or an entire workspace. The
 * effective theme of any terminal surface is resolved most-specific-first:
 *
 *     tab override  ->  pane override  ->  workspace override  ->  global
 *
 * These entry points are invoked from the right-click context menus on a
 * tab label, a pane notebook, and a sidebar workspace row. Each opens a
 * modal dialog listing the available ghostty themes plus an
 * "Inherit (default)" entry that clears that scope's override. Applying
 * the choice updates the scope, re-themes affected live surfaces, and
 * persists to the session file so it survives a restart.
 */
#pragma once

#include <gtk/gtk.h>

#include "ghostty_terminal.h"
#include "workspace.h"

void theme_selector_popup_tab(GtkWidget *anchor, GhosttyTerminal *term);
void theme_selector_popup_pane(GtkWidget *anchor, GtkNotebook *pane);
void theme_selector_popup_workspace(GtkWidget *anchor, Workspace *ws);
