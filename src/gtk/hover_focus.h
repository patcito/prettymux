#pragma once

#include <gtk/gtk.h>

void hover_focus_handle_window_active_changed(gboolean active);
void hover_focus_note_pointer_motion(void);
void hover_focus_suppress_until_motion(void);
gboolean hover_focus_should_enter(void);
