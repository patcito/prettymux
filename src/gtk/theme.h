#pragma once

typedef struct {
    const char *name;
    const char *bg, *fg, *surface, *overlay, *subtext, *accent;
    const char *toast_bg;
    const char *green, *red, *yellow, *blue, *peach, *muted, *highlight;
    const char *status_bar_bg, *status_bar_fg;
} Theme;

#define THEME_COUNT 4

extern int current_theme;

int theme_count(void);
const Theme *theme_get_at(int index);
const Theme *theme_get_current(void);
const char *theme_get_default_tab_accent(const Theme *theme);
void theme_cycle(void);
void theme_apply(void);
void theme_set_by_name(const char *name);
void theme_set_custom(const Theme *theme);
const Theme *theme_get_custom(void);
