#include "close_confirm.h"

#include <string.h>

typedef struct {
    CloseConfirmKind kind;
    CloseConfirmCallback callback;
    gpointer user_data;
    GDestroyNotify destroy;
    GtkWidget *check_button;
} CloseConfirmRequest;

static gboolean settings_loaded = FALSE;
static gboolean confirm_tab_close = TRUE;
static gboolean confirm_pane_close = TRUE;
static gboolean confirm_workspace_close = TRUE;
static gboolean confirm_app_close = TRUE;

static char *
close_confirm_settings_path(void)
{
    return g_build_filename(g_get_home_dir(), ".config", "prettymux",
                            "confirmations.ini", NULL);
}

static void
close_confirm_save(void)
{
    GKeyFile *kf = g_key_file_new();
    char *path = close_confirm_settings_path();
    char *dir = g_path_get_dirname(path);
    char *data;
    gsize len = 0;

    g_key_file_set_boolean(kf, "confirm", "tab_close", confirm_tab_close);
    g_key_file_set_boolean(kf, "confirm", "pane_close", confirm_pane_close);
    g_key_file_set_boolean(kf, "confirm", "workspace_close",
                           confirm_workspace_close);
    g_key_file_set_boolean(kf, "confirm", "app_close", confirm_app_close);

    g_mkdir_with_parents(dir, 0755);
    data = g_key_file_to_data(kf, &len, NULL);
    g_file_set_contents(path, data, (gssize)len, NULL);

    g_free(data);
    g_free(dir);
    g_free(path);
    g_key_file_unref(kf);
}

static void
close_confirm_load(void)
{
    GKeyFile *kf;
    char *path;

    if (settings_loaded)
        return;
    settings_loaded = TRUE;

    kf = g_key_file_new();
    path = close_confirm_settings_path();
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        if (g_key_file_has_key(kf, "confirm", "tab_close", NULL))
            confirm_tab_close = g_key_file_get_boolean(kf, "confirm",
                                                       "tab_close", NULL);
        if (g_key_file_has_key(kf, "confirm", "pane_close", NULL))
            confirm_pane_close = g_key_file_get_boolean(kf, "confirm",
                                                        "pane_close", NULL);
        if (g_key_file_has_key(kf, "confirm", "workspace_close", NULL))
            confirm_workspace_close = g_key_file_get_boolean(kf, "confirm",
                                                             "workspace_close",
                                                             NULL);
        if (g_key_file_has_key(kf, "confirm", "app_close", NULL))
            confirm_app_close = g_key_file_get_boolean(kf, "confirm",
                                                       "app_close", NULL);
    }

    g_free(path);
    g_key_file_unref(kf);
}

gboolean
close_confirm_is_enabled(CloseConfirmKind kind)
{
    close_confirm_load();

    switch (kind) {
    case CLOSE_CONFIRM_TAB:
        return confirm_tab_close;
    case CLOSE_CONFIRM_PANE:
        return confirm_pane_close;
    case CLOSE_CONFIRM_WORKSPACE:
        return confirm_workspace_close;
    case CLOSE_CONFIRM_APP:
        return confirm_app_close;
    default:
        return TRUE;
    }
}

void
close_confirm_set_enabled(CloseConfirmKind kind, gboolean enabled)
{
    close_confirm_load();

    switch (kind) {
    case CLOSE_CONFIRM_TAB:
        confirm_tab_close = enabled;
        break;
    case CLOSE_CONFIRM_PANE:
        confirm_pane_close = enabled;
        break;
    case CLOSE_CONFIRM_WORKSPACE:
        confirm_workspace_close = enabled;
        break;
    case CLOSE_CONFIRM_APP:
        confirm_app_close = enabled;
        break;
    default:
        return;
    }

    close_confirm_save();
}

gboolean
close_confirm_get_enabled(CloseConfirmKind kind)
{
    return close_confirm_is_enabled(kind);
}

void
close_confirm_reset_defaults(void)
{
    close_confirm_load();
    confirm_tab_close = TRUE;
    confirm_pane_close = TRUE;
    confirm_workspace_close = TRUE;
    confirm_app_close = TRUE;
    close_confirm_save();
}

static const char *
close_confirm_heading(CloseConfirmKind kind)
{
    switch (kind) {
    case CLOSE_CONFIRM_TAB:
        return "Close this tab?";
    case CLOSE_CONFIRM_PANE:
        return "Close this pane?";
    case CLOSE_CONFIRM_WORKSPACE:
        return "Close this workspace?";
    case CLOSE_CONFIRM_APP:
        return "Quit PrettyMux?";
    default:
        return "Confirm close";
    }
}

static const char *
close_confirm_body(CloseConfirmKind kind)
{
    switch (kind) {
    case CLOSE_CONFIRM_TAB:
        return "The current tab will be closed.";
    case CLOSE_CONFIRM_PANE:
        return "The current pane and its tabs will be closed.";
    case CLOSE_CONFIRM_WORKSPACE:
        return "The current workspace and its panes will be closed.";
    case CLOSE_CONFIRM_APP:
        return "PrettyMux will close all panes and tabs in this window.";
    default:
        return "";
    }
}

static const char *
close_confirm_label(CloseConfirmKind kind)
{
    switch (kind) {
    case CLOSE_CONFIRM_TAB:
        return "Close Tab";
    case CLOSE_CONFIRM_PANE:
        return "Close Pane";
    case CLOSE_CONFIRM_WORKSPACE:
        return "Close Workspace";
    case CLOSE_CONFIRM_APP:
        return "Quit";
    default:
        return "Confirm";
    }
}

static void
close_confirm_request_free(CloseConfirmRequest *req)
{
    if (req->destroy)
        req->destroy(req->user_data);
    g_free(req);
}

static void
close_confirm_response_cb(AdwMessageDialog *dialog,
                          const char *response,
                          gpointer user_data)
{
    CloseConfirmRequest *req = user_data;
    gboolean confirmed = g_strcmp0(response, "confirm") == 0;

    (void)dialog;

    if (confirmed &&
        req->check_button &&
        gtk_check_button_get_active(GTK_CHECK_BUTTON(req->check_button))) {
        close_confirm_set_enabled(req->kind, FALSE);
    }

    if (req->callback)
        req->callback(confirmed, req->user_data);

    close_confirm_request_free(req);
}

void
close_confirm_request(GtkWindow *parent,
                      CloseConfirmKind kind,
                      CloseConfirmCallback callback,
                      gpointer user_data,
                      GDestroyNotify destroy)
{
    AdwMessageDialog *dialog;
    CloseConfirmRequest *req;
    GtkWidget *check;

    if (!close_confirm_is_enabled(kind) || !parent) {
        if (callback)
            callback(TRUE, user_data);
        if (destroy)
            destroy(user_data);
        return;
    }

    dialog = ADW_MESSAGE_DIALOG(adw_message_dialog_new(
        parent,
        close_confirm_heading(kind),
        close_confirm_body(kind)));
    adw_message_dialog_add_responses(dialog,
                                     "cancel", "Cancel",
                                     "confirm", close_confirm_label(kind),
                                     NULL);
    adw_message_dialog_set_response_appearance(dialog, "confirm",
                                               ADW_RESPONSE_DESTRUCTIVE);
    adw_message_dialog_set_default_response(dialog, "confirm");
    adw_message_dialog_set_close_response(dialog, "cancel");

    check = gtk_check_button_new_with_label(
        "Don't ask again and remember this");
    adw_message_dialog_set_extra_child(dialog, check);

    req = g_new0(CloseConfirmRequest, 1);
    req->kind = kind;
    req->callback = callback;
    req->user_data = user_data;
    req->destroy = destroy;
    req->check_button = check;

    g_signal_connect(dialog, "response",
                     G_CALLBACK(close_confirm_response_cb), req);
    gtk_window_present(GTK_WINDOW(dialog));
}
