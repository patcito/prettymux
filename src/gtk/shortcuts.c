#include "shortcuts.h"

#include <stdio.h>
#include <string.h>

const ShortcutDef default_shortcuts[] = {
    {"workspace.new",     GDK_KEY_n,             GDK_CONTROL_MASK | GDK_SHIFT_MASK, "New workspace"},
    {"workspace.close",   GDK_KEY_d,             GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Close workspace"},
    {"workspace.next",    GDK_KEY_bracketright,  GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Next workspace"},
    {"workspace.prev",    GDK_KEY_bracketleft,   GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Previous workspace"},
    {"workspace.focus.1", GDK_KEY_1,             GDK_CONTROL_MASK,                  "Switch to workspace 1"},
    {"workspace.focus.2", GDK_KEY_2,             GDK_CONTROL_MASK,                  "Switch to workspace 2"},
    {"workspace.focus.3", GDK_KEY_3,             GDK_CONTROL_MASK,                  "Switch to workspace 3"},
    {"workspace.focus.4", GDK_KEY_4,             GDK_CONTROL_MASK,                  "Switch to workspace 4"},
    {"workspace.focus.5", GDK_KEY_5,             GDK_CONTROL_MASK,                  "Switch to workspace 5"},
    {"workspace.focus.6", GDK_KEY_6,             GDK_CONTROL_MASK,                  "Switch to workspace 6"},
    {"workspace.focus.7", GDK_KEY_7,             GDK_CONTROL_MASK,                  "Switch to workspace 7"},
    {"workspace.focus.8", GDK_KEY_8,             GDK_CONTROL_MASK,                  "Switch to workspace 8"},
    {"workspace.focus.9", GDK_KEY_9,             GDK_CONTROL_MASK,                  "Switch to workspace 9"},
    {"pane.tab.new",      GDK_KEY_t,             GDK_CONTROL_MASK | GDK_SHIFT_MASK, "New terminal tab"},
    {"pane.focus.left",   GDK_KEY_Left,          GDK_ALT_MASK,                      "Focus pane left"},
    {"pane.focus.right",  GDK_KEY_Right,         GDK_ALT_MASK,                      "Focus pane right"},
    {"pane.focus.up",     GDK_KEY_Up,            GDK_ALT_MASK,                      "Focus pane up"},
    {"pane.focus.down",   GDK_KEY_Down,          GDK_ALT_MASK,                      "Focus pane down"},
    {"browser.toggle",    GDK_KEY_b,             GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Toggle browser"},
    {"browser.new",       GDK_KEY_p,             GDK_CONTROL_MASK | GDK_SHIFT_MASK, "New browser tab"},
    {"browser.tab.new",   GDK_KEY_t,             GDK_CONTROL_MASK,                  "New browser tab"},
    {"browser.tab.close", GDK_KEY_w,             GDK_CONTROL_MASK,                  "Close browser tab"},
    {"devtools.docked",   GDK_KEY_i,             GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Inspector docked"},
    {"devtools.window",   GDK_KEY_j,             GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Inspector window"},
    {"shortcuts.show",    GDK_KEY_k,             GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Shortcuts overlay"},
    {"search.show",       GDK_KEY_s,             GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Search palette"},
    {"pane.tab.move",     GDK_KEY_g,             GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Move tab to pane"},
    {"tab.close",         GDK_KEY_w,             GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Close tab"},
    {"pane.close",        GDK_KEY_x,             GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Close pane"},
    {"pane.zoom",         GDK_KEY_z,             GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Zoom pane"},
    {"terminal.search",   GDK_KEY_f,             GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Terminal search"},
    {"broadcast.toggle",  GDK_KEY_Return,        GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Broadcast mode"},
    {"notes.toggle",      GDK_KEY_q,             GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Quick notes"},
    {"theme.cycle",       GDK_KEY_comma,         GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Cycle theme"},
    {"history.show",      GDK_KEY_h,             GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Command history"},
    {"pip.toggle",        GDK_KEY_m,             GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Picture in picture"},
    {"split.horizontal",  GDK_KEY_e,             GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Split horizontal"},
    {"split.vertical",    GDK_KEY_o,             GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Split vertical"},
    {"window.fullscreen", GDK_KEY_F11,           0,                                 "Toggle fullscreen"},
    {"browser.focus_url", GDK_KEY_l,             GDK_CONTROL_MASK,                  "Focus URL bar"},
    {"terminal.copy",     GDK_KEY_c,             GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Copy selection"},
    {"terminal.paste",    GDK_KEY_v,             GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Paste"},
    {NULL, 0, 0, NULL},
};

static ShortcutDef *runtime_shortcuts = NULL;
static int runtime_shortcut_count = 0;

static char *
shortcuts_config_path(void)
{
    return g_build_filename(g_get_home_dir(), ".config", "prettymux",
                            "shortcuts.ini", NULL);
}

static GdkModifierType
normalize_mods(GdkModifierType mods)
{
    return mods & (GDK_CONTROL_MASK | GDK_SHIFT_MASK |
                   GDK_ALT_MASK | GDK_SUPER_MASK);
}

static ShortcutDef *
runtime_shortcut_mutable(const char *action)
{
    if (!action || !runtime_shortcuts)
        return NULL;

    for (int i = 0; i < runtime_shortcut_count; i++) {
        if (strcmp(runtime_shortcuts[i].action, action) == 0)
            return &runtime_shortcuts[i];
    }

    return NULL;
}

static int
default_shortcut_count(void)
{
    int count = 0;
    while (default_shortcuts[count].action != NULL)
        count++;
    return count;
}

static void
save_runtime_shortcuts(void)
{
    GKeyFile *kf;
    char *path;
    char *dir;
    char *data;
    gsize len;

    if (!runtime_shortcuts)
        return;

    kf = g_key_file_new();
    for (int i = 0; i < runtime_shortcut_count; i++) {
        char value[64];
        snprintf(value, sizeof(value), "%u:%u",
                 runtime_shortcuts[i].keyval,
                 (unsigned int)normalize_mods(runtime_shortcuts[i].mods));
        g_key_file_set_string(kf, "shortcuts",
                              runtime_shortcuts[i].action, value);
    }

    path = shortcuts_config_path();
    dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    data = g_key_file_to_data(kf, &len, NULL);
    g_file_set_contents(path, data, (gssize)len, NULL);

    g_free(data);
    g_free(dir);
    g_free(path);
    g_key_file_unref(kf);
}

static void
migrate_close_shortcut_swap_if_needed(void)
{
    ShortcutDef *workspace_close = runtime_shortcut_mutable("workspace.close");
    ShortcutDef *tab_close = runtime_shortcut_mutable("tab.close");

    if (!workspace_close || !tab_close)
        return;

    if (workspace_close->keyval == GDK_KEY_w &&
        normalize_mods(workspace_close->mods) ==
            (GDK_CONTROL_MASK | GDK_SHIFT_MASK) &&
        tab_close->keyval == GDK_KEY_d &&
        normalize_mods(tab_close->mods) ==
            (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) {
        workspace_close->keyval = GDK_KEY_d;
        workspace_close->mods = GDK_CONTROL_MASK | GDK_SHIFT_MASK;
        tab_close->keyval = GDK_KEY_w;
        tab_close->mods = GDK_CONTROL_MASK | GDK_SHIFT_MASK;
        save_runtime_shortcuts();
    }
}

void
shortcuts_init(void)
{
    GKeyFile *kf;
    char *path;

    if (runtime_shortcuts)
        return;

    runtime_shortcut_count = default_shortcut_count();
    runtime_shortcuts = g_new0(ShortcutDef, runtime_shortcut_count + 1);
    for (int i = 0; i < runtime_shortcut_count; i++)
        runtime_shortcuts[i] = default_shortcuts[i];

    path = shortcuts_config_path();
    kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        for (int i = 0; i < runtime_shortcut_count; i++) {
            char *value = g_key_file_get_string(kf, "shortcuts",
                                                runtime_shortcuts[i].action,
                                                NULL);
            if (value) {
                unsigned int keyval = 0;
                unsigned int mods = 0;
                if (sscanf(value, "%u:%u", &keyval, &mods) == 2) {
                    runtime_shortcuts[i].keyval = keyval;
                    runtime_shortcuts[i].mods = normalize_mods((GdkModifierType)mods);
                }
                g_free(value);
            }
        }
        migrate_close_shortcut_swap_if_needed();
    }

    g_key_file_unref(kf);
    g_free(path);
}

int
shortcut_count(void)
{
    shortcuts_init();
    return runtime_shortcut_count;
}

const ShortcutDef *
shortcut_get_at(int index)
{
    shortcuts_init();
    if (index < 0 || index >= runtime_shortcut_count)
        return NULL;
    return &runtime_shortcuts[index];
}

const ShortcutDef *
shortcut_find_by_action(const char *action)
{
    shortcuts_init();
    if (!action)
        return NULL;

    for (int i = 0; i < runtime_shortcut_count; i++) {
        if (strcmp(runtime_shortcuts[i].action, action) == 0)
            return &runtime_shortcuts[i];
    }
    return NULL;
}

gboolean
shortcut_set_binding(const char *action, guint keyval,
                     GdkModifierType mods,
                     const ShortcutDef **conflict_out)
{
    GdkModifierType normalized_mods = normalize_mods(mods);

    shortcuts_init();
    if (conflict_out)
        *conflict_out = NULL;
    if (!action || keyval == 0)
        return FALSE;

    for (int i = 0; i < runtime_shortcut_count; i++) {
        if (strcmp(runtime_shortcuts[i].action, action) != 0 &&
            gdk_keyval_to_lower(runtime_shortcuts[i].keyval) ==
                gdk_keyval_to_lower(keyval) &&
            normalize_mods(runtime_shortcuts[i].mods) == normalized_mods) {
            if (conflict_out)
                *conflict_out = &runtime_shortcuts[i];
            return FALSE;
        }
    }

    for (int i = 0; i < runtime_shortcut_count; i++) {
        if (strcmp(runtime_shortcuts[i].action, action) == 0) {
            runtime_shortcuts[i].keyval = keyval;
            runtime_shortcuts[i].mods = normalized_mods;
            save_runtime_shortcuts();
            return TRUE;
        }
    }

    return FALSE;
}

void
shortcut_reset_all(void)
{
    shortcuts_init();
    for (int i = 0; i < runtime_shortcut_count; i++)
        runtime_shortcuts[i] = default_shortcuts[i];

    save_runtime_shortcuts();
}

const char *
shortcut_match(guint keyval, GdkModifierType state)
{
    GdkModifierType mods = normalize_mods(state);
    guint lower = gdk_keyval_to_lower(keyval);

    shortcuts_init();
    for (int i = 0; i < runtime_shortcut_count; i++) {
        if (lower == gdk_keyval_to_lower(runtime_shortcuts[i].keyval) &&
            mods == normalize_mods(runtime_shortcuts[i].mods)) {
            return runtime_shortcuts[i].action;
        }
    }
    return NULL;
}
