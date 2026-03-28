#include "shortcuts.h"

const ShortcutDef default_shortcuts[] = {
    {"workspace.new",     GDK_KEY_n,            GDK_CONTROL_MASK | GDK_SHIFT_MASK, "New workspace"},
    {"workspace.close",   GDK_KEY_w,            GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Close workspace"},
    {"workspace.next",    GDK_KEY_bracketright,  GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Next workspace"},
    {"workspace.prev",    GDK_KEY_bracketleft,   GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Previous workspace"},
    {"pane.tab.new",      GDK_KEY_t,            GDK_CONTROL_MASK | GDK_SHIFT_MASK, "New terminal tab"},
    {"browser.toggle",    GDK_KEY_b,            GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Toggle browser"},
    {"browser.new",       GDK_KEY_p,            GDK_CONTROL_MASK | GDK_SHIFT_MASK, "New browser tab"},
    {"devtools.docked",   GDK_KEY_i,            GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Inspector docked"},
    {"devtools.window",   GDK_KEY_j,            GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Inspector window"},
    {"shortcuts.show",    GDK_KEY_k,            GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Shortcuts overlay"},
    {"search.show",       GDK_KEY_s,            GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Search palette"},
    {"pane.close",        GDK_KEY_x,            GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Close pane"},
    {"pane.zoom",         GDK_KEY_z,            GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Zoom pane"},
    {"terminal.search",   GDK_KEY_f,            GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Terminal search"},
    {"broadcast.toggle",  GDK_KEY_Return,       GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Broadcast mode"},
    {"notes.toggle",      GDK_KEY_q,            GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Quick notes"},
    {"theme.cycle",       GDK_KEY_comma,        GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Cycle theme"},
    {"history.show",      GDK_KEY_h,            GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Command history"},
    {"pip.toggle",        GDK_KEY_m,            GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Picture in picture"},
    {"split.horizontal",  GDK_KEY_e,            GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Split horizontal"},
    {"split.vertical",    GDK_KEY_o,            GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Split vertical"},
    {"browser.focus_url", GDK_KEY_l,            GDK_CONTROL_MASK,                  "Focus URL bar"},
    {"terminal.copy",     GDK_KEY_c,            GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Copy selection"},
    {"terminal.paste",    GDK_KEY_v,            GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Paste"},
    {NULL, 0, 0, NULL},
};

const char *shortcut_match(guint keyval, GdkModifierType state) {
    state &= (GDK_CONTROL_MASK | GDK_SHIFT_MASK | GDK_ALT_MASK | GDK_SUPER_MASK);
    guint lower = gdk_keyval_to_lower(keyval);
    for (int i = 0; default_shortcuts[i].action != NULL; i++) {
        if (lower == gdk_keyval_to_lower(default_shortcuts[i].keyval) &&
            state == default_shortcuts[i].mods) {
            return default_shortcuts[i].action;
        }
    }
    return NULL;
}
