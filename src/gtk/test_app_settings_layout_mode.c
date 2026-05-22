#include "app_settings.h"

#include <glib/gstdio.h>
#include <string.h>

static char *
test_settings_path_for_home(const char *home)
{
    return g_build_filename(home, ".config", "prettymux", "settings.ini",
                            NULL);
}

static void
test_prepare_home(void)
{
    const char *home = g_getenv("PRETTYMUX_TEST_HOME");

    g_assert_nonnull(home);
    g_assert_true(home[0] != '\0');
    g_setenv("HOME", home, TRUE);
    g_mkdir_with_parents(home, 0755);
}

static void
test_write_layout_mode_value(const char *home, const char *layout_mode)
{
    GKeyFile *kf;
    char *path;
    char *dir;
    char *data;

    kf = g_key_file_new();
    g_key_file_set_string(kf, "ui", "default_layout_mode", layout_mode);
    data = g_key_file_to_data(kf, NULL, NULL);

    path = test_settings_path_for_home(home);
    dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    g_assert_true(g_file_set_contents(path, data, -1, NULL));

    g_free(dir);
    g_free(path);
    g_free(data);
    g_key_file_unref(kf);
}

static void
test_assert_saved_layout_mode(const char *home, const char *expected_mode)
{
    GKeyFile *kf;
    char *path;
    char *mode = NULL;

    kf = g_key_file_new();
    path = test_settings_path_for_home(home);
    g_assert_true(g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL));
    mode = g_key_file_get_string(kf, "ui", "default_layout_mode", NULL);
    g_assert_cmpstr(mode, ==, expected_mode);

    g_free(mode);
    g_free(path);
    g_key_file_unref(kf);
}

static void
test_default_layout_mode_fallback_strip(void)
{
    if (g_test_subprocess()) {
        test_prepare_home();
        g_assert_cmpint(app_settings_get_default_layout_mode(), ==,
                        WORKSPACE_LAYOUT_STRIP);
        return;
    }

    char *home = g_dir_make_tmp("prettymux-test-default-layout-XXXXXX", NULL);
    g_assert_nonnull(home);
    g_setenv("PRETTYMUX_TEST_HOME", home, TRUE);
    g_setenv("HOME", home, TRUE);
    g_test_trap_subprocess("/app-settings/layout/default-fallback-strip", 0,
                           0);
    g_test_trap_assert_passed();

    g_unsetenv("PRETTYMUX_TEST_HOME");
    g_free(home);
}

static void
test_default_layout_mode_roundtrip_strip(void)
{
    if (g_test_subprocess()) {
        const char *home;
        const char *phase;

        test_prepare_home();
        home = g_getenv("PRETTYMUX_TEST_HOME");
        phase = g_getenv("PRETTYMUX_TEST_PHASE");
        g_assert_nonnull(phase);

        if (g_strcmp0(phase, "save") == 0) {
            app_settings_set_default_layout_mode(WORKSPACE_LAYOUT_STRIP);
            app_settings_save();
            test_assert_saved_layout_mode(home, "strip");
            return;
        }

        if (g_strcmp0(phase, "load") == 0) {
            g_assert_cmpint(app_settings_get_default_layout_mode(), ==,
                            WORKSPACE_LAYOUT_STRIP);
            return;
        }

        g_error("unknown subprocess phase");
        return;
    }

    char *home = g_dir_make_tmp("prettymux-test-roundtrip-layout-XXXXXX", NULL);
    g_assert_nonnull(home);
    g_setenv("PRETTYMUX_TEST_HOME", home, TRUE);
    g_setenv("HOME", home, TRUE);

    g_setenv("PRETTYMUX_TEST_PHASE", "save", TRUE);
    g_test_trap_subprocess("/app-settings/layout/roundtrip-strip", 0, 0);
    g_test_trap_assert_passed();

    g_setenv("PRETTYMUX_TEST_PHASE", "load", TRUE);
    g_test_trap_subprocess("/app-settings/layout/roundtrip-strip", 0, 0);
    g_test_trap_assert_passed();

    g_unsetenv("PRETTYMUX_TEST_PHASE");
    g_unsetenv("PRETTYMUX_TEST_HOME");
    g_free(home);
}

static void
test_default_layout_mode_invalid_value_fallback(void)
{
    if (g_test_subprocess()) {
        const char *home;

        test_prepare_home();
        home = g_getenv("PRETTYMUX_TEST_HOME");
        test_write_layout_mode_value(home, "not-a-layout-mode");
        g_assert_cmpint(app_settings_get_default_layout_mode(), ==,
                        WORKSPACE_LAYOUT_CLASSIC);
        return;
    }

    char *home = g_dir_make_tmp("prettymux-test-invalid-layout-XXXXXX", NULL);
    g_assert_nonnull(home);
    g_setenv("PRETTYMUX_TEST_HOME", home, TRUE);
    g_setenv("HOME", home, TRUE);
    g_test_trap_subprocess("/app-settings/layout/invalid-value-fallback", 0, 0);
    g_test_trap_assert_passed();

    g_unsetenv("PRETTYMUX_TEST_HOME");
    g_free(home);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/app-settings/layout/default-fallback-strip",
                    test_default_layout_mode_fallback_strip);
    g_test_add_func("/app-settings/layout/roundtrip-strip",
                    test_default_layout_mode_roundtrip_strip);
    g_test_add_func("/app-settings/layout/invalid-value-fallback",
                    test_default_layout_mode_invalid_value_fallback);

    return g_test_run();
}
