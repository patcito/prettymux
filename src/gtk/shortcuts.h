#pragma once

#include <gtk/gtk.h>

typedef struct {
    const char *action;
    guint keyval;
    GdkModifierType mods;
    const char *label;
} ShortcutDef;

// NULL-terminated array of default shortcuts
extern const ShortcutDef default_shortcuts[];

// Match a keypress against shortcuts. Returns action string or NULL.
const char *shortcut_match(guint keyval, GdkModifierType state);
