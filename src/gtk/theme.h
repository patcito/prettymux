#pragma once

typedef struct {
    const char *name;
    const char *bg, *fg, *surface, *overlay, *subtext, *accent;
    const char *green, *red, *yellow, *blue, *peach, *muted, *highlight;
} Theme;

#define THEME_COUNT 3

extern int current_theme;

const Theme *theme_get_current(void);
void theme_cycle(void);
void theme_apply(void);
void theme_set_by_name(const char *name);
