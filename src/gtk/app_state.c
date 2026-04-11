#include "app_state.h"

AppState *
app_state(void)
{
    static AppState state = {
        .ghostty_default_font_size = 0.0f,
        .main_window_active = FALSE,
        .terminal_search_total = -1,
        .terminal_search_selected = -1,
    };

    return &state;
}
