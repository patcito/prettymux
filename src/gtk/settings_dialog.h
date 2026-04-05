#pragma once

#include <gtk/gtk.h>

typedef void (*SettingsDialogApplyCallback)(gpointer user_data);

void settings_dialog_present(GtkWindow *parent,
                             SettingsDialogApplyCallback apply_cb,
                             gpointer user_data);
