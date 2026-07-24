#include "ui_language.h"

#include <glib.h>

static char *settings_language;
static int save_calls;

const char *
app_settings_get_ui_language(void)
{
    return settings_language ? settings_language : "en";
}

void
app_settings_set_ui_language(const char *language_code)
{
    g_free(settings_language);
    settings_language = g_strdup(language_code);
}

void
app_settings_save(void)
{
    save_calls++;
}

static void
test_all_languages_have_visible_labels(void)
{
    for (int i = 0; i < UI_LANGUAGE_COUNT; i++) {
        UiLanguage language = (UiLanguage)i;

        ui_language_set(language);
        g_assert_cmpint(ui_language_current(), ==, language);
        g_assert_nonnull(ui_language_display_name(language));
        g_assert_true(ui_language_display_name(language)[0] != '\0');
        for (int key = 0; key < UI_TEXT_COUNT; key++) {
            const char *translated = ui_text((UiTextKey)key);
            g_assert_nonnull(translated);
            g_assert_true(translated[0] != '\0');
        }
    }
    g_assert_cmpint(save_calls, ==, UI_LANGUAGE_COUNT);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/ui-language/all-visible-labels",
                    test_all_languages_have_visible_labels);
    return g_test_run();
}
