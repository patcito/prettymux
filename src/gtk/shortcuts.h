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

void shortcuts_init(void);
int shortcut_count(void);
const ShortcutDef *shortcut_get_at(int index);
const ShortcutDef *shortcut_find_by_action(const char *action);
gboolean shortcut_set_binding(const char *action, guint keyval,
                              GdkModifierType mods,
                              const ShortcutDef **conflict_out);
void shortcut_reset_all(void);
char *shortcut_format_binding(const ShortcutDef *binding);
void shortcut_log_event(const char *type, const char *action,
                        const char *keys);

// Match a keypress against shortcuts. Returns action string or NULL.
const char *shortcut_match(guint keyval, GdkModifierType state);
