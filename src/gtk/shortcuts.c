#include "shortcuts.h"

#include <stdio.h>
#include <string.h>

const ShortcutDef default_shortcuts[] = {
    {"workspace.new",     GDK_KEY_n,             GDK_CONTROL_MASK | GDK_SHIFT_MASK, "New workspace"},
    {"workspace.close",   GDK_KEY_d,             GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Close workspace"},
    {"workspace.next",    GDK_KEY_bracketright,  GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Next workspace"},
    {"workspace.prev",    GDK_KEY_bracketleft,   GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Previous workspace"},
    {"workspace.focus.1", GDK_KEY_1,             GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Switch to workspace 1"},
    {"workspace.focus.2", GDK_KEY_2,             GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Switch to workspace 2"},
    {"workspace.focus.3", GDK_KEY_3,             GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Switch to workspace 3"},
    {"workspace.focus.4", GDK_KEY_4,             GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Switch to workspace 4"},
    {"workspace.focus.5", GDK_KEY_5,             GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Switch to workspace 5"},
    {"workspace.focus.6", GDK_KEY_6,             GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Switch to workspace 6"},
    {"workspace.focus.7", GDK_KEY_7,             GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Switch to workspace 7"},
    {"workspace.focus.8", GDK_KEY_8,             GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Switch to workspace 8"},
    {"workspace.focus.9", GDK_KEY_9,             GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Switch to workspace 9"},
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
    {"settings.show",     GDK_KEY_s,             GDK_CONTROL_MASK | GDK_ALT_MASK,   "Settings"},
    {"theme.cycle",       GDK_KEY_comma,         GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Cycle theme"},
    {"history.show",      GDK_KEY_h,             GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Command history"},
    {"pip.toggle",        GDK_KEY_m,             GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Picture in picture"},
    {"split.horizontal",  GDK_KEY_e,             GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Split horizontal"},
    {"split.vertical",    GDK_KEY_o,             GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Split vertical"},
    {"window.fullscreen", GDK_KEY_F11,           0,                                 "Toggle fullscreen"},
    {"browser.focus_url", GDK_KEY_l,             GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Focus URL bar"},
    {"terminal.copy",     GDK_KEY_c,             GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Copy selection"},
    {"terminal.paste",    GDK_KEY_v,             GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Paste"},
    {"recording.mark",    GDK_KEY_y,             GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Recording marker"},
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

static guint
normalize_keyval(guint keyval)
{
    switch (keyval) {
    case GDK_KEY_less:        return GDK_KEY_comma;
    case GDK_KEY_greater:     return GDK_KEY_period;
    case GDK_KEY_colon:       return GDK_KEY_semicolon;
    case GDK_KEY_question:    return GDK_KEY_slash;
    case GDK_KEY_bar:         return GDK_KEY_backslash;
    case GDK_KEY_underscore:  return GDK_KEY_minus;
    case GDK_KEY_plus:        return GDK_KEY_equal;
    case GDK_KEY_braceleft:   return GDK_KEY_bracketleft;
    case GDK_KEY_braceright:  return GDK_KEY_bracketright;
    case GDK_KEY_quotedbl:    return GDK_KEY_apostrophe;
    case GDK_KEY_asciitilde:  return GDK_KEY_grave;
    case GDK_KEY_exclam:      return GDK_KEY_1;
    case GDK_KEY_at:          return GDK_KEY_2;
    case GDK_KEY_numbersign:  return GDK_KEY_3;
    case GDK_KEY_dollar:      return GDK_KEY_4;
    case GDK_KEY_percent:     return GDK_KEY_5;
    case GDK_KEY_asciicircum: return GDK_KEY_6;
    case GDK_KEY_ampersand:   return GDK_KEY_7;
    case GDK_KEY_asterisk:    return GDK_KEY_8;
    case GDK_KEY_parenleft:   return GDK_KEY_9;
    case GDK_KEY_parenright:  return GDK_KEY_0;
    case GDK_KEY_ISO_Enter:   return GDK_KEY_Return;
    case GDK_KEY_KP_Enter:    return GDK_KEY_Return;
    case GDK_KEY_KP_1:        return GDK_KEY_1;
    case GDK_KEY_KP_2:        return GDK_KEY_2;
    case GDK_KEY_KP_3:        return GDK_KEY_3;
    case GDK_KEY_KP_4:        return GDK_KEY_4;
    case GDK_KEY_KP_5:        return GDK_KEY_5;
    case GDK_KEY_KP_6:        return GDK_KEY_6;
    case GDK_KEY_KP_7:        return GDK_KEY_7;
    case GDK_KEY_KP_8:        return GDK_KEY_8;
    case GDK_KEY_KP_9:        return GDK_KEY_9;
    case GDK_KEY_KP_0:        return GDK_KEY_0;
    case GDK_KEY_KP_End:      return GDK_KEY_1;
    case GDK_KEY_KP_Down:     return GDK_KEY_2;
    case GDK_KEY_KP_Page_Down:return GDK_KEY_3;
    case GDK_KEY_KP_Left:     return GDK_KEY_4;
    case GDK_KEY_KP_Begin:    return GDK_KEY_5;
    case GDK_KEY_KP_Right:    return GDK_KEY_6;
    case GDK_KEY_KP_Home:     return GDK_KEY_7;
    case GDK_KEY_KP_Up:       return GDK_KEY_8;
    case GDK_KEY_KP_Page_Up:  return GDK_KEY_9;
    case GDK_KEY_KP_Insert:   return GDK_KEY_0;
    default:                  return gdk_keyval_to_lower(keyval);
    }
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

static void
migrate_shortcut_updates_if_needed(void)
{
    gboolean changed = FALSE;
    ShortcutDef *browser_focus = runtime_shortcut_mutable("browser.focus_url");

    for (int i = 1; i <= 9; i++) {
        char action[32];
        ShortcutDef *workspace_focus;

        snprintf(action, sizeof(action), "workspace.focus.%d", i);
        workspace_focus = runtime_shortcut_mutable(action);
        if (!workspace_focus)
            continue;

        if (normalize_keyval(workspace_focus->keyval) == (guint)(GDK_KEY_0 + i) &&
            normalize_mods(workspace_focus->mods) == GDK_CONTROL_MASK) {
            workspace_focus->keyval = GDK_KEY_0 + i;
            workspace_focus->mods = GDK_CONTROL_MASK | GDK_SHIFT_MASK;
            changed = TRUE;
        }
    }

    if (browser_focus &&
        normalize_keyval(browser_focus->keyval) == GDK_KEY_l &&
        normalize_mods(browser_focus->mods) == GDK_CONTROL_MASK) {
        browser_focus->keyval = GDK_KEY_l;
        browser_focus->mods = GDK_CONTROL_MASK | GDK_SHIFT_MASK;
        changed = TRUE;
    }

    if (changed)
        save_runtime_shortcuts();
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
        migrate_shortcut_updates_if_needed();
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
            normalize_keyval(runtime_shortcuts[i].keyval) ==
                normalize_keyval(keyval) &&
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

char *
shortcut_format_binding(const ShortcutDef *binding)
{
    GString *s;
    const char *name;
    char pretty[32];

    if (!binding)
        return g_strdup("");

    s = g_string_new(NULL);
    if (binding->mods & GDK_CONTROL_MASK)
        g_string_append(s, "Ctrl+");
    if (binding->mods & GDK_SHIFT_MASK)
        g_string_append(s, "Shift+");
    if (binding->mods & GDK_ALT_MASK)
        g_string_append(s, "Alt+");
    if (binding->mods & GDK_SUPER_MASK)
        g_string_append(s, "Super+");

    name = gdk_keyval_name(normalize_keyval(binding->keyval));
    if (!name)
        return g_string_free(s, FALSE);

    if (strcmp(name, "Return") == 0)
        snprintf(pretty, sizeof(pretty), "Enter");
    else if (strcmp(name, "bracketleft") == 0)
        snprintf(pretty, sizeof(pretty), "[");
    else if (strcmp(name, "bracketright") == 0)
        snprintf(pretty, sizeof(pretty), "]");
    else if (strcmp(name, "comma") == 0)
        snprintf(pretty, sizeof(pretty), ",");
    else if (strcmp(name, "Left") == 0 || strcmp(name, "Right") == 0 ||
             strcmp(name, "Up") == 0 || strcmp(name, "Down") == 0 ||
             strcmp(name, "Tab") == 0 || strcmp(name, "Escape") == 0)
        snprintf(pretty, sizeof(pretty), "%s", name);
    else {
        snprintf(pretty, sizeof(pretty), "%s", name);
        if (pretty[0] >= 'a' && pretty[0] <= 'z')
            pretty[0] = pretty[0] - 'a' + 'A';
    }

    g_string_append(s, pretty);
    return g_string_free(s, FALSE);
}

const char *
shortcut_match(guint keyval, GdkModifierType state)
{
    GdkModifierType mods = normalize_mods(state);
    guint normalized = normalize_keyval(keyval);

    shortcuts_init();
    for (int i = 0; i < runtime_shortcut_count; i++) {
        if (normalized == normalize_keyval(runtime_shortcuts[i].keyval) &&
            mods == normalize_mods(runtime_shortcuts[i].mods)) {
            return runtime_shortcuts[i].action;
        }
    }
    return NULL;
}
