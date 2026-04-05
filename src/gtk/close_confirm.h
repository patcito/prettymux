#pragma once

#include <adwaita.h>

typedef enum {
    CLOSE_CONFIRM_TAB,
    CLOSE_CONFIRM_PANE,
    CLOSE_CONFIRM_WORKSPACE,
    CLOSE_CONFIRM_APP,
} CloseConfirmKind;

typedef void (*CloseConfirmCallback)(gboolean confirmed, gpointer user_data);

void close_confirm_request(GtkWindow *parent,
                           CloseConfirmKind kind,
                           CloseConfirmCallback callback,
                           gpointer user_data,
                           GDestroyNotify destroy);

gboolean close_confirm_get_enabled(CloseConfirmKind kind);
void close_confirm_set_enabled(CloseConfirmKind kind, gboolean enabled);
void close_confirm_reset_defaults(void);
