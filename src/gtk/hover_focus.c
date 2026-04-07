#include "hover_focus.h"

static gboolean hover_window_active = FALSE;
static gboolean hover_blocked_until_motion = FALSE;
static gboolean hover_active_initialized = FALSE;

void
hover_focus_handle_window_active_changed(gboolean active)
{
    active = active != FALSE;

    if (!hover_active_initialized) {
        hover_window_active = active;
        hover_active_initialized = TRUE;
        return;
    }

    if (!hover_window_active && active)
        hover_blocked_until_motion = TRUE;

    hover_window_active = active;
}

void
hover_focus_note_pointer_motion(void)
{
    if (!hover_window_active)
        return;

    hover_blocked_until_motion = FALSE;
}

gboolean
hover_focus_should_enter(void)
{
    return hover_window_active && !hover_blocked_until_motion;
}
