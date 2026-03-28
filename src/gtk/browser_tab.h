#pragma once

#include <gtk/gtk.h>
#include <webkit/webkit.h>

G_BEGIN_DECLS

#define BROWSER_TYPE_TAB (browser_tab_get_type())
G_DECLARE_FINAL_TYPE(BrowserTab, browser_tab, BROWSER, TAB, GtkBox)

GtkWidget *browser_tab_new(const char *url);

const char *browser_tab_get_url(BrowserTab *self);
const char *browser_tab_get_title(BrowserTab *self);

void browser_tab_navigate(BrowserTab *self, const char *url);
void browser_tab_go_back(BrowserTab *self);
void browser_tab_go_forward(BrowserTab *self);
void browser_tab_reload(BrowserTab *self);
void browser_tab_show_inspector(BrowserTab *self);

/* URL history / autocomplete */
void browser_tab_add_url_to_history(const char *url);
GPtrArray *browser_tab_get_url_history(void);
void browser_tab_set_url_history(GPtrArray *history);
void browser_tab_focus_url(BrowserTab *self);

G_END_DECLS
