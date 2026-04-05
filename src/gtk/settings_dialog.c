#include "settings_dialog.h"

#include "app_settings.h"
#include "close_confirm.h"
#include "theme.h"

#include <string.h>

typedef struct {
    SettingsDialogApplyCallback apply_cb;
    gpointer user_data;
    GtkWidget *theme_dropdown;
    GtkWidget *toast_position_dropdown;
    GtkWidget *font_spin;
    GtkWidget *ghostty_theme_entry;
    GtkWidget *confirm_tab;
    GtkWidget *confirm_pane;
    GtkWidget *confirm_workspace;
    GtkWidget *confirm_app;
    GtkWidget *custom_group;
    GtkWidget *color_buttons[14];
} SettingsDialogState;

enum {
    COLOR_BG = 0,
    COLOR_FG,
    COLOR_SURFACE,
    COLOR_OVERLAY,
    COLOR_SUBTEXT,
    COLOR_ACCENT,
    COLOR_TOAST_BG,
    COLOR_GREEN,
    COLOR_RED,
    COLOR_YELLOW,
    COLOR_BLUE,
    COLOR_PEACH,
    COLOR_MUTED,
    COLOR_HIGHLIGHT,
    COLOR_COUNT
};

static const struct {
    const char *label;
    const char *key;
} color_fields[COLOR_COUNT] = {
    {"Background", "bg"},
    {"Foreground", "fg"},
    {"Surface", "surface"},
    {"Overlay", "overlay"},
    {"Subtext", "subtext"},
    {"Tab accent", "accent"},
    {"Toast background", "toast_bg"},
    {"Green", "green"},
    {"Red", "red"},
    {"Yellow", "yellow"},
    {"Blue", "blue"},
    {"Peach", "peach"},
    {"Muted", "muted"},
    {"Selection highlight", "highlight"},
};

enum {
    GHOSTTY_THEME_COLUMN_NAME = 0,
};

static int
ghostty_theme_token_start(const char *text, int cursor_pos)
{
    int start;

    if (!text)
        return 0;

    start = CLAMP(cursor_pos, 0, (int)strlen(text));
    while (start > 0) {
        char ch = text[start - 1];
        if (ch == ':' || ch == ',')
            break;
        start--;
    }

    while (text[start] == ' ')
        start++;

    return start;
}

static GtkTreeModel *
ghostty_theme_completion_model(void)
{
    static GtkListStore *store = NULL;

    if (!store) {
        gchar *stdout_data = NULL;
        gchar *stderr_data = NULL;
        gchar **lines;

        store = gtk_list_store_new(1, G_TYPE_STRING);
        if (!g_spawn_command_line_sync("ghostty +list-themes",
                                       &stdout_data, &stderr_data, NULL, NULL)) {
            static const char *fallback_themes[] = {
                "Catppuccin Mocha",
                "Catppuccin Latte",
                "Adwaita Dark",
                "Adwaita",
                NULL
            };

            for (int i = 0; fallback_themes[i]; i++) {
                GtkTreeIter iter;
                gtk_list_store_append(store, &iter);
                gtk_list_store_set(store, &iter,
                                   GHOSTTY_THEME_COLUMN_NAME, fallback_themes[i],
                                   -1);
            }
            return GTK_TREE_MODEL(store);
        }

        lines = g_strsplit(stdout_data ? stdout_data : "", "\n", -1);
        for (guint i = 0; lines[i] != NULL; i++) {
            GtkTreeIter iter;
            char *line = g_strstrip(lines[i]);
            char *suffix;

            if (!line[0])
                continue;

            suffix = strstr(line, " (");
            if (suffix)
                *suffix = '\0';

            gtk_list_store_append(store, &iter);
            gtk_list_store_set(store, &iter,
                               GHOSTTY_THEME_COLUMN_NAME, line,
                               -1);
        }

        g_strfreev(lines);
        g_free(stdout_data);
        g_free(stderr_data);
    }

    return GTK_TREE_MODEL(store);
}

static gboolean
ghostty_theme_match_func(GtkEntryCompletion *completion,
                         const char *key,
                         GtkTreeIter *iter,
                         gpointer user_data)
{
    GtkWidget *entry;
    const char *text;
    char *candidate = NULL;
    char *token = NULL;
    char *candidate_fold = NULL;
    char *token_fold = NULL;
    int cursor_pos;
    int token_start;
    gboolean match = FALSE;

    (void)key;
    (void)user_data;

    entry = gtk_entry_completion_get_entry(completion);
    if (!entry)
        return FALSE;

    text = gtk_editable_get_text(GTK_EDITABLE(entry));
    cursor_pos = gtk_editable_get_position(GTK_EDITABLE(entry));
    token_start = ghostty_theme_token_start(text, cursor_pos);
    token = g_strndup(text + token_start, cursor_pos - token_start);

    gtk_tree_model_get(gtk_entry_completion_get_model(completion), iter,
                       GHOSTTY_THEME_COLUMN_NAME, &candidate,
                       -1);

    candidate_fold = g_utf8_casefold(candidate ? candidate : "", -1);
    token_fold = g_utf8_casefold(token ? token : "", -1);
    match = token_fold[0] == '\0' || g_str_has_prefix(candidate_fold, token_fold);

    g_free(candidate);
    g_free(token);
    g_free(candidate_fold);
    g_free(token_fold);
    return match;
}

static gboolean
on_ghostty_theme_match_selected(GtkEntryCompletion *completion,
                                GtkTreeModel *model,
                                GtkTreeIter *iter,
                                gpointer user_data)
{
    GtkWidget *entry;
    const char *text;
    char *theme_name = NULL;
    int cursor_pos;
    int token_start;
    int insert_pos;

    (void)user_data;

    entry = gtk_entry_completion_get_entry(completion);
    if (!entry)
        return FALSE;

    text = gtk_editable_get_text(GTK_EDITABLE(entry));
    cursor_pos = gtk_editable_get_position(GTK_EDITABLE(entry));
    token_start = ghostty_theme_token_start(text, cursor_pos);

    gtk_tree_model_get(model, iter,
                       GHOSTTY_THEME_COLUMN_NAME, &theme_name,
                       -1);
    if (!theme_name)
        return FALSE;

    gtk_editable_delete_text(GTK_EDITABLE(entry), token_start, cursor_pos);
    insert_pos = token_start;
    gtk_editable_insert_text(GTK_EDITABLE(entry), theme_name, -1, &insert_pos);
    gtk_editable_set_position(GTK_EDITABLE(entry), insert_pos);

    g_free(theme_name);
    return TRUE;
}

static gboolean
theme_name_is_custom(const char *name)
{
    return g_strcmp0(name, "Custom") == 0;
}

static void
rgba_from_hex(const char *hex, GdkRGBA *rgba)
{
    if (!rgba)
        return;
    if (!hex || !gdk_rgba_parse(rgba, hex))
        gdk_rgba_parse(rgba, "#000000");
}

static char *
hex_from_rgba(const GdkRGBA *rgba)
{
    int red = CLAMP((int)(rgba->red * 255.0 + 0.5), 0, 255);
    int green = CLAMP((int)(rgba->green * 255.0 + 0.5), 0, 255);
    int blue = CLAMP((int)(rgba->blue * 255.0 + 0.5), 0, 255);
    return g_strdup_printf("#%02x%02x%02x", red, green, blue);
}

static GtkWidget *
settings_section_title(const char *title, const char *subtitle)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *title_label = gtk_label_new(title);
    GtkWidget *subtitle_label = gtk_label_new(subtitle);

    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 4);
    gtk_label_set_xalign(GTK_LABEL(title_label), 0.0f);
    gtk_label_set_xalign(GTK_LABEL(subtitle_label), 0.0f);
    gtk_widget_add_css_class(title_label, "heading");
    gtk_widget_add_css_class(subtitle_label, "dim-label");
    gtk_label_set_wrap(GTK_LABEL(subtitle_label), TRUE);

    gtk_box_append(GTK_BOX(box), title_label);
    gtk_box_append(GTK_BOX(box), subtitle_label);
    return box;
}

static GtkWidget *
settings_row(const char *label, GtkWidget *control)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *lbl = gtk_label_new(label);

    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
    gtk_widget_set_hexpand(lbl, TRUE);
    gtk_widget_set_halign(control, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(box), lbl);
    gtk_box_append(GTK_BOX(box), control);
    return box;
}

static void
settings_update_custom_visibility(SettingsDialogState *state)
{
    guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(state->theme_dropdown));
    const Theme *theme = theme_get_at((int)selected);
    gboolean is_custom = theme && theme_name_is_custom(theme->name);
    gtk_widget_set_visible(state->custom_group, is_custom);

    if (theme && !is_custom) {
        gtk_editable_set_text(GTK_EDITABLE(state->ghostty_theme_entry),
                              app_settings_default_ghostty_theme_for_prettymux_theme(theme->name));
    }
}

static void
on_theme_selection_changed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    (void)object;
    (void)pspec;
    settings_update_custom_visibility(user_data);
}

static void
on_reset_confirmations_clicked(GtkButton *button, gpointer user_data)
{
    SettingsDialogState *state = user_data;
    (void)button;

    gtk_switch_set_active(GTK_SWITCH(state->confirm_tab), TRUE);
    gtk_switch_set_active(GTK_SWITCH(state->confirm_pane), TRUE);
    gtk_switch_set_active(GTK_SWITCH(state->confirm_workspace), TRUE);
    gtk_switch_set_active(GTK_SWITCH(state->confirm_app), TRUE);
}

static Theme
settings_collect_custom_theme(SettingsDialogState *state)
{
    Theme theme = *app_settings_get_custom_theme();

    for (int i = 0; i < COLOR_COUNT; i++) {
        GdkRGBA rgba;
        char *hex;

        rgba = *gtk_color_dialog_button_get_rgba(
            GTK_COLOR_DIALOG_BUTTON(state->color_buttons[i]));
        hex = hex_from_rgba(&rgba);

        switch (i) {
        case COLOR_BG:        theme.bg = hex; break;
        case COLOR_FG:        theme.fg = hex; break;
        case COLOR_SURFACE:   theme.surface = hex; break;
        case COLOR_OVERLAY:   theme.overlay = hex; break;
        case COLOR_SUBTEXT:   theme.subtext = hex; break;
        case COLOR_ACCENT:    theme.accent = hex; break;
        case COLOR_TOAST_BG:  theme.toast_bg = hex; break;
        case COLOR_GREEN:     theme.green = hex; break;
        case COLOR_RED:       theme.red = hex; break;
        case COLOR_YELLOW:    theme.yellow = hex; break;
        case COLOR_BLUE:      theme.blue = hex; break;
        case COLOR_PEACH:     theme.peach = hex; break;
        case COLOR_MUTED:     theme.muted = hex; break;
        case COLOR_HIGHLIGHT: theme.highlight = hex; break;
        }
    }

    theme.name = "Custom";
    return theme;
}

static void
hide_settings_window(GtkWindow *dialog)
{
    if (!dialog)
        return;
    gtk_widget_set_visible(GTK_WIDGET(dialog), FALSE);
}

static gboolean
on_settings_close_request(GtkWindow *window, gpointer user_data)
{
    (void)user_data;
    hide_settings_window(window);
    return TRUE;
}

static void
on_apply_clicked(GtkButton *button, gpointer user_data)
{
    SettingsDialogState *state = user_data;
    guint selected;
    guint toast_position_selected;
    const Theme *selected_theme;
    Theme custom_theme;
    GtkWindow *dialog;

    (void)button;
    close_confirm_set_enabled(CLOSE_CONFIRM_TAB,
        gtk_switch_get_active(GTK_SWITCH(state->confirm_tab)));
    close_confirm_set_enabled(CLOSE_CONFIRM_PANE,
        gtk_switch_get_active(GTK_SWITCH(state->confirm_pane)));
    close_confirm_set_enabled(CLOSE_CONFIRM_WORKSPACE,
        gtk_switch_get_active(GTK_SWITCH(state->confirm_workspace)));
    close_confirm_set_enabled(CLOSE_CONFIRM_APP,
        gtk_switch_get_active(GTK_SWITCH(state->confirm_app)));

    app_settings_set_ghostty_font_size(
        gtk_spin_button_get_value(GTK_SPIN_BUTTON(state->font_spin)));
    app_settings_set_ghostty_theme(
        gtk_editable_get_text(GTK_EDITABLE(state->ghostty_theme_entry)));
    toast_position_selected =
        gtk_drop_down_get_selected(GTK_DROP_DOWN(state->toast_position_dropdown));
    app_settings_set_toast_position(
        toast_position_selected == 1 ? "bottom" : "top");

    selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(state->theme_dropdown));
    selected_theme = theme_get_at((int)selected);

    if (selected_theme && theme_name_is_custom(selected_theme->name)) {
        custom_theme = settings_collect_custom_theme(state);
        app_settings_set_custom_theme(&custom_theme);
        theme_set_custom(&custom_theme);
    }

    if (selected_theme)
        theme_set_by_name(selected_theme->name);

    app_settings_save();

    if (state->apply_cb)
        state->apply_cb(state->user_data);

    dialog = GTK_WINDOW(g_object_get_data(G_OBJECT(button), "settings-dialog"));
    hide_settings_window(dialog);
}

void
settings_dialog_present(GtkWindow *parent,
                        SettingsDialogApplyCallback apply_cb,
                        gpointer user_data)
{
    static const char *theme_names[] = {"Dark", "Light", "Monokai", "Custom", NULL};
    static const char *toast_positions[] = {"Top", "Bottom", NULL};
    const Theme *custom_theme = app_settings_get_custom_theme();
    const Theme *current_theme = theme_get_current();
    const char *toast_position = app_settings_get_toast_position();
    guint current_theme_index = 0;
    SettingsDialogState *state = g_new0(SettingsDialogState, 1);
    GtkWindow *dialog = GTK_WINDOW(gtk_window_new());
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *reset_button;
    GtkWidget *apply_button;
    GtkWidget *close_button;
    GtkWidget *grid;
    GtkColorDialog *color_dialog = gtk_color_dialog_new();

    state->apply_cb = apply_cb;
    state->user_data = user_data;

    for (int i = 0; i < theme_count(); i++) {
        const Theme *candidate = theme_get_at(i);
        if (candidate && g_strcmp0(candidate->name, current_theme->name) == 0) {
            current_theme_index = (guint)i;
            break;
        }
    }

    gtk_window_set_title(dialog, "Settings");
    gtk_window_set_transient_for(dialog, parent);
    gtk_window_set_modal(dialog, TRUE);
    gtk_window_set_default_size(dialog, 720, 760);
    g_signal_connect(dialog, "close-request",
                     G_CALLBACK(on_settings_close_request), NULL);

    gtk_widget_set_margin_top(outer, 18);
    gtk_widget_set_margin_bottom(outer, 18);
    gtk_widget_set_margin_start(outer, 18);
    gtk_widget_set_margin_end(outer, 18);

    gtk_widget_set_margin_top(content, 16);
    gtk_widget_set_margin_bottom(content, 16);
    gtk_widget_set_margin_start(content, 16);
    gtk_widget_set_margin_end(content, 16);

    gtk_box_append(GTK_BOX(content),
                   settings_section_title("Close confirmations",
                                          "Choose which destructive actions should always ask before closing."));

    state->confirm_tab = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(state->confirm_tab),
                          close_confirm_get_enabled(CLOSE_CONFIRM_TAB));
    gtk_box_append(GTK_BOX(content),
                   settings_row("Ask before closing a tab", state->confirm_tab));

    state->confirm_pane = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(state->confirm_pane),
                          close_confirm_get_enabled(CLOSE_CONFIRM_PANE));
    gtk_box_append(GTK_BOX(content),
                   settings_row("Ask before closing a pane", state->confirm_pane));

    state->confirm_workspace = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(state->confirm_workspace),
                          close_confirm_get_enabled(CLOSE_CONFIRM_WORKSPACE));
    gtk_box_append(GTK_BOX(content),
                   settings_row("Ask before closing a workspace", state->confirm_workspace));

    state->confirm_app = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(state->confirm_app),
                          close_confirm_get_enabled(CLOSE_CONFIRM_APP));
    gtk_box_append(GTK_BOX(content),
                   settings_row("Ask before quitting PrettyMux", state->confirm_app));

    reset_button = gtk_button_new_with_label("Reset confirmations");
    g_signal_connect(reset_button, "clicked",
                     G_CALLBACK(on_reset_confirmations_clicked), state);
    gtk_box_append(GTK_BOX(content), reset_button);

    gtk_box_append(GTK_BOX(content),
                   settings_section_title("Ghostty defaults",
                                          "Applied to embedded terminals. Theme accepts Ghostty theme syntax, including dark/light pairs."));

    state->font_spin = gtk_spin_button_new_with_range(0.0, 72.0, 0.5);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(state->font_spin), 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->font_spin),
                              app_settings_get_ghostty_font_size());
    gtk_box_append(GTK_BOX(content),
                   settings_row("Default font size (0 keeps Ghostty default)",
                                state->font_spin));

    state->ghostty_theme_entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(state->ghostty_theme_entry),
                          app_settings_get_ghostty_theme());
    gtk_entry_set_placeholder_text(GTK_ENTRY(state->ghostty_theme_entry),
                                   "dark:Catppuccin Mocha,light:Catppuccin Latte");
    {
        GtkEntryCompletion *completion = gtk_entry_completion_new();
        gtk_entry_completion_set_model(completion, ghostty_theme_completion_model());
        gtk_entry_completion_set_text_column(completion, GHOSTTY_THEME_COLUMN_NAME);
        gtk_entry_completion_set_minimum_key_length(completion, 1);
        gtk_entry_completion_set_inline_completion(completion, TRUE);
        gtk_entry_completion_set_popup_completion(completion, TRUE);
        gtk_entry_completion_set_popup_set_width(completion, TRUE);
        gtk_entry_completion_set_match_func(completion,
                                            ghostty_theme_match_func,
                                            NULL, NULL);
        g_signal_connect(completion, "match-selected",
                         G_CALLBACK(on_ghostty_theme_match_selected), NULL);
        gtk_entry_set_completion(GTK_ENTRY(state->ghostty_theme_entry), completion);
        g_object_unref(completion);
    }
    gtk_box_append(GTK_BOX(content),
                   settings_row("Default Ghostty theme",
                                state->ghostty_theme_entry));

    gtk_box_append(GTK_BOX(content),
                   settings_section_title("PrettyMux theme",
                                          "Pick a built-in theme or customize a cohesive palette for the GTK chrome and tabs. Tab accent controls the active terminal tab underline and activity color."));

    state->theme_dropdown = gtk_drop_down_new_from_strings(theme_names);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(state->theme_dropdown),
                               current_theme_index);
    g_signal_connect(state->theme_dropdown, "notify::selected",
                     G_CALLBACK(on_theme_selection_changed), state);
    gtk_box_append(GTK_BOX(content),
                   settings_row("Theme", state->theme_dropdown));

    state->toast_position_dropdown = gtk_drop_down_new_from_strings(toast_positions);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(state->toast_position_dropdown),
                               g_strcmp0(toast_position, "bottom") == 0 ? 1 : 0);
    gtk_box_append(GTK_BOX(content),
                   settings_row("Toast position",
                                state->toast_position_dropdown));

    state->custom_group = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);

    for (int i = 0; i < COLOR_COUNT; i++) {
        GtkWidget *label = gtk_label_new(color_fields[i].label);
        GtkWidget *button = gtk_color_dialog_button_new(color_dialog);
        GdkRGBA rgba;
        const char *hex = NULL;

        switch (i) {
        case COLOR_BG:        hex = custom_theme->bg; break;
        case COLOR_FG:        hex = custom_theme->fg; break;
        case COLOR_SURFACE:   hex = custom_theme->surface; break;
        case COLOR_OVERLAY:   hex = custom_theme->overlay; break;
        case COLOR_SUBTEXT:   hex = custom_theme->subtext; break;
        case COLOR_ACCENT:    hex = custom_theme->accent; break;
        case COLOR_TOAST_BG:  hex = custom_theme->toast_bg; break;
        case COLOR_GREEN:     hex = custom_theme->green; break;
        case COLOR_RED:       hex = custom_theme->red; break;
        case COLOR_YELLOW:    hex = custom_theme->yellow; break;
        case COLOR_BLUE:      hex = custom_theme->blue; break;
        case COLOR_PEACH:     hex = custom_theme->peach; break;
        case COLOR_MUTED:     hex = custom_theme->muted; break;
        case COLOR_HIGHLIGHT: hex = custom_theme->highlight; break;
        }

        rgba_from_hex(hex, &rgba);
        gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(button), &rgba);
        gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
        gtk_grid_attach(GTK_GRID(grid), label, (i % 2) * 2, i / 2, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), button, (i % 2) * 2 + 1, i / 2, 1, 1);
        state->color_buttons[i] = button;
    }

    gtk_box_append(GTK_BOX(state->custom_group), grid);
    gtk_box_append(GTK_BOX(content), state->custom_group);
    settings_update_custom_visibility(state);

    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), content);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(outer), scroll);

    close_button = gtk_button_new_with_label("Cancel");
    g_signal_connect_swapped(close_button, "clicked",
                             G_CALLBACK(hide_settings_window), dialog);
    gtk_box_append(GTK_BOX(buttons), close_button);

    apply_button = gtk_button_new_with_label("Apply");
    g_object_set_data(G_OBJECT(apply_button), "settings-dialog", dialog);
    g_signal_connect(apply_button, "clicked",
                     G_CALLBACK(on_apply_clicked), state);
    gtk_box_append(GTK_BOX(buttons), apply_button);

    gtk_widget_set_halign(buttons, GTK_ALIGN_END);
    gtk_widget_set_margin_top(buttons, 8);
    gtk_box_append(GTK_BOX(outer), buttons);

    gtk_window_set_child(dialog, outer);
    g_object_set_data(G_OBJECT(dialog), "settings-state", state);
    gtk_window_present(dialog);
}
