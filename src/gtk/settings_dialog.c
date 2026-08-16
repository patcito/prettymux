#include "settings_dialog.h"

#include "app_settings.h"
#include "close_confirm.h"
#include "theme.h"
#include "theme_selector.h"
#include "workspace.h"

#include <string.h>

typedef struct {
    SettingsDialogApplyCallback apply_cb;
    gpointer user_data;
    GtkWindow *dialog;
    GtkWidget *scroll;
    GtkWidget *content;
    GtkWidget *theme_dropdown;
    GtkWidget *toast_position_dropdown;
    GtkWidget *focus_on_hover_switch;
    GtkWidget *copy_on_select_switch;
    GtkWidget *layout_mode_dropdown;
    GtkWidget *gtk_renderer_dropdown;
    GtkWidget *font_spin;
    GtkWidget *tab_height_spin;
    GtkWidget *ghostty_theme_dropdown;
    GtkWidget *confirm_tab;
    GtkWidget *confirm_pane;
    GtkWidget *confirm_workspace;
    GtkWidget *confirm_app;
    GtkWidget *custom_group;
    GtkWidget *color_buttons[16];
} SettingsDialogState;

static GtkWindow *g_settings_dialog = NULL;

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
    COLOR_STATUS_BAR_BG,
    COLOR_STATUS_BAR_FG,
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
    {"Status bar background", "status_bar_bg"},
    {"Status bar text", "status_bar_fg"},
};


/* Select `name` in the ghostty-theme dropdown, appending it to the model if
 * it isn't one of the enumerated themes (e.g. a compound "dark:X,light:Y"
 * value the user typed previously), so switching selections never loses it. */
static void
settings_ghostty_theme_dropdown_select(GtkDropDown *dd, const char *name)
{
    if (!dd || !name || !name[0])
        return;
    GtkStringList *model = GTK_STRING_LIST(gtk_drop_down_get_model(dd));
    guint n = g_list_model_get_n_items(G_LIST_MODEL(model));
    for (guint i = 0; i < n; i++) {
        if (g_strcmp0(gtk_string_list_get_string(model, i), name) == 0) {
            gtk_drop_down_set_selected(dd, i);
            return;
        }
    }
    gtk_string_list_append(model, name);
    gtk_drop_down_set_selected(dd, n);
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

static gboolean
settings_scroll_custom_group_idle(gpointer user_data)
{
    SettingsDialogState *state = user_data;
    GtkAdjustment *adj;
    graphene_rect_t bounds;
    double lower;
    double upper;
    double target;

    if (!state || !state->scroll || !state->content || !state->custom_group ||
        !gtk_widget_get_visible(state->custom_group))
        return G_SOURCE_REMOVE;

    adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(state->scroll));
    if (!adj)
        return G_SOURCE_REMOVE;

    if (gtk_widget_compute_bounds(state->custom_group, state->content, &bounds)) {
        lower = gtk_adjustment_get_lower(adj);
        upper = gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj);
        if (upper < lower)
            upper = lower;
        target = CLAMP(bounds.origin.y - 24.0, lower, upper);
        gtk_adjustment_set_value(adj, target);
    }

    if (state->color_buttons[0])
        gtk_widget_grab_focus(state->color_buttons[0]);

    return G_SOURCE_REMOVE;
}

static void
settings_update_custom_visibility(SettingsDialogState *state)
{
    guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(state->theme_dropdown));
    const Theme *theme = theme_get_at((int)selected);
    gboolean is_custom = theme && theme_name_is_custom(theme->name);
    gtk_widget_set_visible(state->custom_group, is_custom);

    if (is_custom) {
        if (state->dialog)
            gtk_window_set_default_size(state->dialog, 720, 900);
        g_idle_add(settings_scroll_custom_group_idle, state);
    } else if (state->dialog) {
        gtk_window_set_default_size(state->dialog, 720, 760);
    }

}

/* Point the Ghostty theme at the default that pairs with the selected
 * PrettyMux chrome theme. Only ever called for a USER-driven chrome-theme
 * change: doing it while building the dialog would overwrite (and then Apply
 * would persist) whatever Ghostty theme the user already had saved. */
static void
settings_sync_ghostty_theme_to_chrome(SettingsDialogState *state)
{
    guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(state->theme_dropdown));
    const Theme *theme = theme_get_at((int)selected);

    if (theme && !theme_name_is_custom(theme->name)) {
        settings_ghostty_theme_dropdown_select(
            GTK_DROP_DOWN(state->ghostty_theme_dropdown),
            app_settings_default_ghostty_theme_for_prettymux_theme(theme->name));
    }
}

static void
on_theme_selection_changed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    (void)object;
    (void)pspec;
    settings_update_custom_visibility(user_data);
    settings_sync_ghostty_theme_to_chrome(user_data);
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

        gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(state->color_buttons[i]),
                                   &rgba);
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
        case COLOR_STATUS_BAR_BG: theme.status_bar_bg = hex; break;
        case COLOR_STATUS_BAR_FG: theme.status_bar_fg = hex; break;
        }
    }

    theme.name = "Custom";
    return theme;
}

static void
settings_dialog_destroy(GtkWindow *dialog)
{
    if (!dialog)
        return;
    gtk_window_destroy(dialog);
}

static void
on_settings_dialog_destroy(GtkWindow *window, gpointer user_data)
{
    SettingsDialogState *state = user_data;

    if (g_settings_dialog == window)
        g_settings_dialog = NULL;

    g_free(state);
}

static gboolean
on_settings_close_request(GtkWindow *window, gpointer user_data)
{
    (void)user_data;
    settings_dialog_destroy(window);
    return TRUE;
}

static void
on_apply_clicked(GtkButton *button, gpointer user_data)
{
    SettingsDialogState *state = user_data;
    guint selected;
    guint toast_position_selected;
    guint layout_mode_selected;
    guint renderer_selected;
    const char *renderer_mode;
    WorkspaceLayoutMode previous_default_layout_mode;
    WorkspaceLayoutMode default_layout_mode;
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
    app_settings_set_tab_height(
        gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(state->tab_height_spin)));
    {
        GObject *sel = gtk_drop_down_get_selected_item(
            GTK_DROP_DOWN(state->ghostty_theme_dropdown));
        if (sel)
            app_settings_set_ghostty_theme(
                gtk_string_object_get_string(GTK_STRING_OBJECT(sel)));
    }
    toast_position_selected =
        gtk_drop_down_get_selected(GTK_DROP_DOWN(state->toast_position_dropdown));
    app_settings_set_toast_position(
        toast_position_selected == 1 ? "bottom" : "top");
    app_settings_set_focus_on_hover(
        gtk_switch_get_active(GTK_SWITCH(state->focus_on_hover_switch)));
    app_settings_set_copy_on_select(
        gtk_switch_get_active(GTK_SWITCH(state->copy_on_select_switch)));
    previous_default_layout_mode = app_settings_get_default_layout_mode();
    layout_mode_selected =
        gtk_drop_down_get_selected(GTK_DROP_DOWN(state->layout_mode_dropdown));
    default_layout_mode = layout_mode_selected == 1
        ? WORKSPACE_LAYOUT_STRIP
        : WORKSPACE_LAYOUT_CLASSIC;
    app_settings_set_default_layout_mode(default_layout_mode);
    if (default_layout_mode != previous_default_layout_mode)
        workspace_apply_layout_mode_to_all(default_layout_mode);
    renderer_selected =
        gtk_drop_down_get_selected(GTK_DROP_DOWN(state->gtk_renderer_dropdown));
    renderer_mode = renderer_selected == 1 ? "vulkan"
        : renderer_selected == 2      ? "opengl"
        : renderer_selected == 3      ? "ngl"
                                     : "auto";
    app_settings_set_gtk_renderer_mode(renderer_mode);

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
    settings_dialog_destroy(dialog);
}

void
settings_dialog_present(GtkWindow *parent,
                        SettingsDialogApplyCallback apply_cb,
                        gpointer user_data)
{
    static const char *theme_names[] = {"Dark", "Light", "Monokai", "Custom", NULL};
    static const char *toast_positions[] = {"Top", "Bottom", NULL};
    static const char *layout_mode_options[] = {
        "Classic (split panes)",
        "Strip (horizontal columns)",
        NULL
    };
    static const char *gtk_renderer_modes[] = {"Auto (prefer Vulkan)", "Vulkan", "OpenGL", "NGL", NULL};
    const Theme *custom_theme = app_settings_get_custom_theme();
    const Theme *current_theme = theme_get_current();
    const char *toast_position = app_settings_get_toast_position();
    WorkspaceLayoutMode default_layout_mode =
        app_settings_get_default_layout_mode();
    const char *renderer_mode = app_settings_get_gtk_renderer_mode();
    guint current_theme_index = 0;
    guint layout_mode_index = 0;
    guint renderer_mode_index = 0;
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

    state->apply_cb = apply_cb;
    state->user_data = user_data;
    state->dialog = dialog;
    state->scroll = scroll;
    state->content = content;

    if (g_settings_dialog) {
        gtk_window_present(g_settings_dialog);
        g_free(state);
        return;
    }

    g_settings_dialog = dialog;

    for (int i = 0; i < theme_count(); i++) {
        const Theme *candidate = theme_get_at(i);
        if (candidate && g_strcmp0(candidate->name, current_theme->name) == 0) {
            current_theme_index = (guint)i;
            break;
        }
    }

    if (g_strcmp0(renderer_mode, "vulkan") == 0)
        renderer_mode_index = 1;
    else if (g_strcmp0(renderer_mode, "opengl") == 0)
        renderer_mode_index = 2;
    else if (g_strcmp0(renderer_mode, "ngl") == 0)
        renderer_mode_index = 3;
    if (default_layout_mode == WORKSPACE_LAYOUT_STRIP)
        layout_mode_index = 1;

    gtk_window_set_title(dialog, "Settings");
    gtk_window_set_transient_for(dialog, parent);
    gtk_window_set_modal(dialog, TRUE);
    gtk_window_set_default_size(dialog, 720, 760);
    g_signal_connect(dialog, "close-request",
                     G_CALLBACK(on_settings_close_request), NULL);
    g_signal_connect(dialog, "destroy",
                     G_CALLBACK(on_settings_dialog_destroy), state);

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

    {
        /* Searchable dropdown of the full ghostty theme list (same source as
         * the right-click scoped pickers), preselecting the current value. */
        GtkStringList *theme_model = gtk_string_list_new(NULL);
        char **theme_names = theme_selector_list_themes();
        for (int i = 0; theme_names[i]; i++)
            gtk_string_list_append(theme_model, theme_names[i]);

        GtkExpression *expr = gtk_property_expression_new(
            GTK_TYPE_STRING_OBJECT, NULL, "string");
        state->ghostty_theme_dropdown =
            gtk_drop_down_new(G_LIST_MODEL(theme_model), expr);
        gtk_drop_down_set_enable_search(
            GTK_DROP_DOWN(state->ghostty_theme_dropdown), TRUE);
        settings_ghostty_theme_dropdown_select(
            GTK_DROP_DOWN(state->ghostty_theme_dropdown),
            app_settings_get_ghostty_theme());
    }
    gtk_box_append(GTK_BOX(content),
                   settings_row("Default Ghostty theme (terminal colors)",
                                state->ghostty_theme_dropdown));

    state->focus_on_hover_switch = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(state->focus_on_hover_switch),
                          app_settings_get_focus_on_hover());
    gtk_box_append(GTK_BOX(content),
                   settings_row("Focus panes on hover",
                                state->focus_on_hover_switch));

    state->copy_on_select_switch = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(state->copy_on_select_switch),
                          app_settings_get_copy_on_select());
    gtk_box_append(GTK_BOX(content),
                   settings_row("Copy on select",
                                state->copy_on_select_switch));

    state->gtk_renderer_dropdown = gtk_drop_down_new_from_strings(gtk_renderer_modes);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(state->gtk_renderer_dropdown),
                               renderer_mode_index);
    gtk_widget_set_tooltip_text(state->gtk_renderer_dropdown,
                                "Auto probes Vulkan once on this machine and caches a compatible GPU renderer choice.");
    gtk_box_append(GTK_BOX(content),
                   settings_row("GTK renderer",
                                state->gtk_renderer_dropdown));

    gtk_box_append(GTK_BOX(content),
                   settings_section_title("Workspace defaults",
                                          "Choose how workspaces use split panes or strip columns. Applying this updates current workspaces and the default for new ones."));

    state->layout_mode_dropdown =
        gtk_drop_down_new_from_strings(layout_mode_options);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(state->layout_mode_dropdown),
                               layout_mode_index);
    gtk_box_append(GTK_BOX(content),
                   settings_row("Default layout mode",
                                state->layout_mode_dropdown));

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

    state->tab_height_spin = gtk_spin_button_new_with_range(24, 64, 1);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(state->tab_height_spin), 0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->tab_height_spin),
                              app_settings_get_tab_height());
    gtk_box_append(GTK_BOX(content),
                   settings_row("Tab bar height (px)",
                                state->tab_height_spin));

    state->custom_group = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);

    for (int i = 0; i < COLOR_COUNT; i++) {
        GtkWidget *label = gtk_label_new(color_fields[i].label);
        GtkWidget *button = gtk_color_button_new();
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
        case COLOR_STATUS_BAR_BG: hex = custom_theme->status_bar_bg; break;
        case COLOR_STATUS_BAR_FG: hex = custom_theme->status_bar_fg; break;
        }

        rgba_from_hex(hex, &rgba);
        gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(button), &rgba);
        gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(button), FALSE);
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
                             G_CALLBACK(settings_dialog_destroy), dialog);
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
    gtk_window_present(dialog);
}
