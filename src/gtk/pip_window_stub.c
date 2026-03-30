#include "pip_window.h"

PipWindow *
pip_window_new(GtkWindow *parent G_GNUC_UNUSED,
               GtkWidget *browser_notebook G_GNUC_UNUSED)
{
    return NULL;
}

void
pip_window_close(PipWindow *pip G_GNUC_UNUSED)
{
}

gboolean
pip_window_is_active(void)
{
    return FALSE;
}

void
pip_window_toggle(GtkWindow *parent G_GNUC_UNUSED,
                  GtkWidget *browser_notebook G_GNUC_UNUSED)
{
}
