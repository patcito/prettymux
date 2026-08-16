/*
 * test_shortcuts_unique.c - guard against duplicate default keybindings.
 *
 * A collision makes a shortcut unreachable: shortcut_match() returns the FIRST
 * entry whose (keyval, mods) match, so a later action silently never fires.
 * This actually happened -- a new theme shortcut was added on Ctrl+Shift+D,
 * which workspace.close already owned.
 *
 * The check runs through shortcut_match() rather than comparing the raw table,
 * so it also catches collisions that only appear after the runtime's keyval /
 * modifier normalization (e.g. KP_Enter vs Return, shifted punctuation).
 * HOME is redirected to a temp dir first so a developer's real
 * ~/.config/prettymux/shortcuts.ini cannot influence the result.
 */
#include <gtk/gtk.h>

#include "shortcuts.h"

static void
test_default_bindings_are_reachable(void)
{
    for (int i = 0; default_shortcuts[i].action != NULL; i++) {
        const ShortcutDef *def = &default_shortcuts[i];
        if (def->keyval == 0)
            continue;

        const char *matched = shortcut_match(def->keyval, def->mods);
        g_assert_nonnull(matched);
        if (g_strcmp0(matched, def->action) != 0) {
            g_error("shortcut collision: '%s' (keyval=%u mods=0x%x) is shadowed "
                    "by '%s'",
                    def->action, def->keyval, (unsigned)def->mods, matched);
        }
    }
}

int
main(int argc, char **argv)
{
    char *tmp_home;

    g_test_init(&argc, &argv, NULL);

    /* Hermetic: never read the developer's real shortcuts.ini. */
    tmp_home = g_dir_make_tmp("prettymux-shortcuts-XXXXXX", NULL);
    g_assert_nonnull(tmp_home);
    g_setenv("HOME", tmp_home, TRUE);
    g_free(tmp_home);

    g_test_add_func("/shortcuts/default-bindings-reachable",
                    test_default_bindings_are_reachable);
    return g_test_run();
}
