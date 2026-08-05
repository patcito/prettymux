#include "desktop_toolbar.h"

#include <gtk/gtk.h>

#include "app_state.h"
#include "ui_language.h"

static const char *last_action;
static int picker_calls;
static UiLanguage selected_language = UI_LANGUAGE_EN;

AppState *
app_state(void)
{
    static AppState state;
    return &state;
}

void
app_actions_handle(const char *action)
{
    last_action = action;
}

void
workspace_picker_show(GtkWindow *parent)
{
    (void)parent;
    picker_calls++;
}

void
sidebar_ui_update_language(void)
{
}

UiLanguage
ui_language_current(void)
{
    return selected_language;
}

void
ui_language_set(UiLanguage language)
{
    selected_language = language;
}

const char *
ui_language_display_name(UiLanguage language)
{
    static const char *names[] = {
        "中文", "English", "한국어", "日本語", "Español",
    };
    return names[language];
}

const char *
ui_text(UiTextKey key)
{
    static const char *labels[UI_TEXT_COUNT] = {
        [UI_TEXT_LANGUAGE] = "Language",
        [UI_TEXT_NEW_PROJECT] = "New Project",
        [UI_TEXT_NEW_TAB] = "New Tab",
        [UI_TEXT_RIGHT_TERMINAL] = "Right Terminal",
        [UI_TEXT_BELOW_TERMINAL] = "Below Terminal",
        [UI_TEXT_CLOSE_PANE] = "Close Pane",
        [UI_TEXT_ZOOM_RESTORE] = "Zoom",
        [UI_TEXT_ALL_ACTIONS] = "All Actions",
    };
    return labels[key] ? labels[key] : "";
}

static void
test_toolbar_exposes_common_actions(void)
{
    GtkWidget *scroll = desktop_toolbar_new();
    GtkWidget *toolbar =
        g_object_get_data(G_OBJECT(scroll), "prettymux-toolbar");
    const char *expected[] = {
        "pane.tab.new",
        "split.horizontal",
        "split.vertical",
        "pane.close",
        "pane.zoom",
        "search.show",
    };
    guint action_index = 0;

    g_assert_true(GTK_IS_BOX(toolbar));
    for (GtkWidget *child = gtk_widget_get_first_child(toolbar);
         child;
         child = gtk_widget_get_next_sibling(child)) {
        const char *role =
            g_object_get_data(G_OBJECT(child), "prettymux-role");
        const char *action =
            g_object_get_data(G_OBJECT(child), "prettymux-action");

        g_assert_false(gtk_widget_get_focusable(child));
        if (g_strcmp0(role, "new-project") == 0) {
            g_signal_emit_by_name(child, "clicked");
            g_assert_cmpint(picker_calls, ==, 1);
            continue;
        }
        if (!GTK_IS_BUTTON(child))
            continue;

        g_assert_cmpuint(action_index, <, G_N_ELEMENTS(expected));
        g_assert_cmpstr(action, ==, expected[action_index++]);
        last_action = NULL;
        g_signal_emit_by_name(child, "clicked");
        g_assert_cmpstr(last_action, ==, action);
    }

    g_assert_cmpuint(action_index, ==, G_N_ELEMENTS(expected));

    {
        GtkWidget *dropdown = g_object_get_data(
            G_OBJECT(scroll), "prettymux-language-dropdown");
        g_assert_true(GTK_IS_DROP_DOWN(dropdown));
        gtk_drop_down_set_selected(GTK_DROP_DOWN(dropdown), UI_LANGUAGE_JA);
        g_assert_cmpint(selected_language, ==, UI_LANGUAGE_JA);
    }
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    if (!gtk_init_check())
        return 0;

    g_test_add_func("/desktop-toolbar/common-actions",
                    test_toolbar_exposes_common_actions);
    return g_test_run();
}
