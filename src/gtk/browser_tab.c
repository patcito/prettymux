#include "browser_tab.h"

#include <string.h>
#include <ctype.h>

/* ---------- URL history (module-level singleton) ---------- */

static GPtrArray *url_history = NULL;  /* array of g_strdup'd URLs */

/* ---------- type definition ---------- */

struct _BrowserTab {
    GtkBox parent_instance;

    GtkWidget *back_btn;
    GtkWidget *fwd_btn;
    GtkWidget *reload_btn;
    GtkWidget *url_entry;
    WebKitWebView *web_view;
    gboolean autocomplete_active; /* guard to prevent recursive edits */
};

G_DEFINE_TYPE(BrowserTab, browser_tab, GTK_TYPE_BOX)

enum {
    SIGNAL_TITLE_CHANGED,
    SIGNAL_NEW_TAB_REQUESTED,
    N_SIGNALS,
};

static guint signals[N_SIGNALS];

/* ---------- helpers ---------- */

/* Build a navigable URL from raw user input.
 *   - Already has a scheme   -> use as-is
 *   - Contains a dot, no spaces -> prepend https://
 *   - Otherwise              -> Google search                            */
static char *
url_from_input(const char *text)
{
    if (g_str_has_prefix(text, "http://") ||
        g_str_has_prefix(text, "https://") ||
        g_str_has_prefix(text, "file://")) {
        return g_strdup(text);
    }

    if (strchr(text, '.') != NULL && strchr(text, ' ') == NULL) {
        return g_strconcat("https://", text, NULL);
    }

    char *encoded = g_uri_escape_string(text, NULL, FALSE);
    char *url = g_strconcat("https://www.google.com/search?q=", encoded, NULL);
    g_free(encoded);
    return url;
}

/* ---------- signal / callback handlers ---------- */

static void
on_back_clicked(GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
    BrowserTab *self = BROWSER_TAB(user_data);
    webkit_web_view_go_back(self->web_view);
}

static void
on_fwd_clicked(GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
    BrowserTab *self = BROWSER_TAB(user_data);
    webkit_web_view_go_forward(self->web_view);
}

static void
on_reload_clicked(GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
    BrowserTab *self = BROWSER_TAB(user_data);
    webkit_web_view_reload(self->web_view);
}

static void
on_url_entry_activate(GtkEntry *entry G_GNUC_UNUSED, gpointer user_data)
{
    BrowserTab *self = BROWSER_TAB(user_data);
    const char *text = gtk_editable_get_text(GTK_EDITABLE(self->url_entry));

    /* Trim leading/trailing whitespace. */
    g_autofree char *trimmed = g_strstrip(g_strdup(text));
    if (trimmed[0] == '\0')
        return;

    g_autofree char *url = url_from_input(trimmed);
    webkit_web_view_load_uri(self->web_view, url);
}

static void
on_uri_changed(WebKitWebView *web_view,
               GParamSpec *pspec G_GNUC_UNUSED,
               gpointer user_data)
{
    BrowserTab *self = BROWSER_TAB(user_data);
    const char *uri = webkit_web_view_get_uri(web_view);
    if (uri != NULL) {
        /* Guard: prevent autocomplete from running on programmatic changes */
        self->autocomplete_active = TRUE;
        gtk_editable_set_text(GTK_EDITABLE(self->url_entry), uri);
        self->autocomplete_active = FALSE;
        browser_tab_add_url_to_history(uri);
    }
}

/* ---------- URL autocomplete on text edit ---------- */

static void
on_url_entry_changed(GtkEditable *editable, gpointer user_data)
{
    BrowserTab *self = BROWSER_TAB(user_data);

    if (self->autocomplete_active)
        return;

    const char *text = gtk_editable_get_text(editable);
    if (!text || text[0] == '\0')
        return;
    if (!url_history)
        return;

    int text_len = (int)strlen(text);
    guint i;

    /* Try exact prefix match from history */
    for (i = 0; i < url_history->len; i++) {
        const char *entry = g_ptr_array_index(url_history, i);
        if (g_ascii_strncasecmp(entry, text, text_len) == 0) {
            self->autocomplete_active = TRUE;
            gtk_editable_set_text(editable, entry);
            gtk_editable_select_region(editable, text_len, (int)strlen(entry));
            self->autocomplete_active = FALSE;
            return;
        }
    }

    /* Try matching without protocol (user types "gith..." matches "https://github.com") */
    for (i = 0; i < url_history->len; i++) {
        const char *entry = g_ptr_array_index(url_history, i);
        const char *stripped = entry;
        if (g_str_has_prefix(stripped, "https://"))
            stripped += 8;
        else if (g_str_has_prefix(stripped, "http://"))
            stripped += 7;
        if (g_ascii_strncasecmp(stripped, text, text_len) == 0) {
            self->autocomplete_active = TRUE;
            gtk_editable_set_text(editable, entry);
            gtk_editable_select_region(editable, text_len,
                                       (int)strlen(entry));
            self->autocomplete_active = FALSE;
            return;
        }
    }
}

static void
on_title_changed(WebKitWebView *web_view,
                 GParamSpec *pspec G_GNUC_UNUSED,
                 gpointer user_data)
{
    BrowserTab *self = BROWSER_TAB(user_data);
    const char *title = webkit_web_view_get_title(web_view);
    if (title != NULL)
        g_signal_emit(self, signals[SIGNAL_TITLE_CHANGED], 0, title);
}

/* Handle target=_blank: WebKit fires "create" and expects a new web view.
 * We return a temporary web view, wait for it to start loading, then emit
 * our "new-tab-requested" signal with the destination URL and destroy the
 * temporary view. */
static void
on_temp_view_load_changed(WebKitWebView *temp_view,
                          WebKitLoadEvent load_event,
                          gpointer user_data)
{
    if (load_event != WEBKIT_LOAD_STARTED)
        return;

    BrowserTab *self = BROWSER_TAB(user_data);
    const char *uri = webkit_web_view_get_uri(temp_view);

    if (uri != NULL && g_strcmp0(uri, "about:blank") != 0)
        g_signal_emit(self, signals[SIGNAL_NEW_TAB_REQUESTED], 0, uri);

    /* Schedule destruction after the signal handler returns. */
    GtkWidget *widget = GTK_WIDGET(temp_view);
    g_idle_add_once((GSourceOnceFunc) gtk_widget_unparent, widget);
}

static GtkWidget *
on_create_new_view(WebKitWebView *web_view G_GNUC_UNUSED,
                   WebKitNavigationAction *action G_GNUC_UNUSED,
                   gpointer user_data)
{
    BrowserTab *self = BROWSER_TAB(user_data);

    /* The temporary view must share the related-view so that the request
     * context (cookies, etc.) is inherited. */
    WebKitWebView *temp = WEBKIT_WEB_VIEW(
        g_object_new(WEBKIT_TYPE_WEB_VIEW,
                     "related-view", self->web_view,
                     NULL));

    g_signal_connect(temp, "load-changed",
                     G_CALLBACK(on_temp_view_load_changed), self);

    return GTK_WIDGET(temp);
}

/* ---------- Inspector embed handlers ---------- */

static gboolean
on_inspector_attach(WebKitWebInspector *inspector, gpointer user_data)
{
    BrowserTab *self = BROWSER_TAB(user_data);
    GtkWidget *inspector_view = GTK_WIDGET(webkit_web_inspector_get_web_view(inspector));

    gtk_widget_set_size_request(inspector_view, -1, 350);
    gtk_widget_set_vexpand(inspector_view, FALSE);

    /* Set 1.5x zoom on the inspector view for bigger fonts */
    WebKitWebView *iview = WEBKIT_WEB_VIEW(webkit_web_inspector_get_web_view(inspector));
    if (iview) {
        WebKitSettings *isettings = webkit_web_view_get_settings(iview);
        webkit_settings_set_default_font_size(isettings, 24);
    }

    gtk_box_append(GTK_BOX(self), inspector_view);
    return TRUE;
}

static gboolean
on_inspector_detach(WebKitWebInspector *inspector, gpointer user_data)
{
    BrowserTab *self = BROWSER_TAB(user_data);
    GtkWidget *inspector_view = GTK_WIDGET(webkit_web_inspector_get_web_view(inspector));

    if (inspector_view && gtk_widget_get_parent(inspector_view) == GTK_WIDGET(self))
        gtk_box_remove(GTK_BOX(self), inspector_view);
    return FALSE;
}

static gboolean
on_inspector_closed(WebKitWebInspector *inspector, gpointer user_data)
{
    BrowserTab *self = BROWSER_TAB(user_data);
    GtkWidget *inspector_view = GTK_WIDGET(webkit_web_inspector_get_web_view(inspector));

    if (inspector_view && gtk_widget_get_parent(inspector_view) == GTK_WIDGET(self))
        gtk_box_remove(GTK_BOX(self), inspector_view);
    return FALSE;
}

/* ---------- GObject boilerplate ---------- */

static void
browser_tab_dispose(GObject *object)
{
    G_OBJECT_CLASS(browser_tab_parent_class)->dispose(object);
}

static void
browser_tab_class_init(BrowserTabClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = browser_tab_dispose;

    /**
     * BrowserTab::title-changed:
     * @self: the browser tab
     * @title: the new page title
     *
     * Emitted when the page title changes.
     */
    signals[SIGNAL_TITLE_CHANGED] =
        g_signal_new("title-changed",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_LAST,
                     0, NULL, NULL, NULL,
                     G_TYPE_NONE, 1, G_TYPE_STRING);

    /**
     * BrowserTab::new-tab-requested:
     * @self: the browser tab
     * @url: the URL to open in a new tab
     *
     * Emitted when a link with target=_blank is activated.
     */
    signals[SIGNAL_NEW_TAB_REQUESTED] =
        g_signal_new("new-tab-requested",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_LAST,
                     0, NULL, NULL, NULL,
                     G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
browser_tab_init(BrowserTab *self)
{
    gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
    gtk_box_set_spacing(GTK_BOX(self), 0);

    /* --- address bar --- */
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_start(bar, 4);
    gtk_widget_set_margin_end(bar, 4);
    gtk_widget_set_margin_top(bar, 4);
    gtk_widget_set_margin_bottom(bar, 4);

    self->back_btn = gtk_button_new_with_label("<");
    self->fwd_btn = gtk_button_new_with_label(">");
    self->reload_btn = gtk_button_new_with_label("R");

    self->url_entry = gtk_entry_new();
    gtk_widget_set_hexpand(self->url_entry, TRUE);

    gtk_box_append(GTK_BOX(bar), self->back_btn);
    gtk_box_append(GTK_BOX(bar), self->fwd_btn);
    gtk_box_append(GTK_BOX(bar), self->reload_btn);
    gtk_box_append(GTK_BOX(bar), self->url_entry);

    gtk_box_append(GTK_BOX(self), bar);

    /* --- web view --- */
    self->web_view = WEBKIT_WEB_VIEW(webkit_web_view_new());
    gtk_widget_set_vexpand(GTK_WIDGET(self->web_view), TRUE);
    gtk_widget_set_hexpand(GTK_WIDGET(self->web_view), TRUE);

    /* Enable developer tools (Web Inspector) with 2x font size */
    WebKitSettings *settings = webkit_web_view_get_settings(self->web_view);
    webkit_settings_set_enable_developer_extras(settings, TRUE);

    gtk_box_append(GTK_BOX(self), GTK_WIDGET(self->web_view));

    /* Wire inspector signals for embedded attach */
    WebKitWebInspector *inspector = webkit_web_view_get_inspector(self->web_view);
    g_signal_connect(inspector, "attach", G_CALLBACK(on_inspector_attach), self);
    g_signal_connect(inspector, "detach", G_CALLBACK(on_inspector_detach), self);
    g_signal_connect(inspector, "closed", G_CALLBACK(on_inspector_closed), self);

    /* --- connect signals --- */
    g_signal_connect(self->back_btn, "clicked",
                     G_CALLBACK(on_back_clicked), self);
    g_signal_connect(self->fwd_btn, "clicked",
                     G_CALLBACK(on_fwd_clicked), self);
    g_signal_connect(self->reload_btn, "clicked",
                     G_CALLBACK(on_reload_clicked), self);
    g_signal_connect(self->url_entry, "activate",
                     G_CALLBACK(on_url_entry_activate), self);
    g_signal_connect(GTK_EDITABLE(self->url_entry), "changed",
                     G_CALLBACK(on_url_entry_changed), self);

    g_signal_connect(self->web_view, "notify::uri",
                     G_CALLBACK(on_uri_changed), self);
    g_signal_connect(self->web_view, "notify::title",
                     G_CALLBACK(on_title_changed), self);
    g_signal_connect(self->web_view, "create",
                     G_CALLBACK(on_create_new_view), self);
}

/* ---------- public API ---------- */

GtkWidget *
browser_tab_new(const char *url)
{
    BrowserTab *self = g_object_new(BROWSER_TYPE_TAB, NULL);

    if (url != NULL && url[0] != '\0') {
        g_autofree char *resolved = url_from_input(url);
        gtk_editable_set_text(GTK_EDITABLE(self->url_entry), resolved);
        webkit_web_view_load_uri(self->web_view, resolved);
    }

    return GTK_WIDGET(self);
}

const char *
browser_tab_get_url(BrowserTab *self)
{
    g_return_val_if_fail(BROWSER_IS_TAB(self), NULL);
    return webkit_web_view_get_uri(self->web_view);
}

const char *
browser_tab_get_title(BrowserTab *self)
{
    g_return_val_if_fail(BROWSER_IS_TAB(self), NULL);
    return webkit_web_view_get_title(self->web_view);
}

void
browser_tab_navigate(BrowserTab *self, const char *url)
{
    g_return_if_fail(BROWSER_IS_TAB(self));
    g_return_if_fail(url != NULL);

    g_autofree char *resolved = url_from_input(url);
    gtk_editable_set_text(GTK_EDITABLE(self->url_entry), resolved);
    webkit_web_view_load_uri(self->web_view, resolved);
}

void
browser_tab_go_back(BrowserTab *self)
{
    g_return_if_fail(BROWSER_IS_TAB(self));
    webkit_web_view_go_back(self->web_view);
}

void
browser_tab_go_forward(BrowserTab *self)
{
    g_return_if_fail(BROWSER_IS_TAB(self));
    webkit_web_view_go_forward(self->web_view);
}

void
browser_tab_reload(BrowserTab *self)
{
    g_return_if_fail(BROWSER_IS_TAB(self));
    webkit_web_view_reload(self->web_view);
}

void
browser_tab_show_inspector(BrowserTab *self)
{
    g_return_if_fail(BROWSER_IS_TAB(self));
    WebKitWebInspector *inspector = webkit_web_view_get_inspector(self->web_view);
    webkit_web_inspector_attach(inspector);
    webkit_web_inspector_show(inspector);
}

void
browser_tab_focus_url(BrowserTab *self)
{
    g_return_if_fail(BROWSER_IS_TAB(self));
    gtk_widget_grab_focus(self->url_entry);
    gtk_editable_select_region(GTK_EDITABLE(self->url_entry), 0, -1);
}

/* ---------- URL history management ---------- */

void
browser_tab_add_url_to_history(const char *url)
{
    if (!url || url[0] == '\0')
        return;
    /* Skip about: and data: URIs */
    if (g_str_has_prefix(url, "about:") || g_str_has_prefix(url, "data:"))
        return;

    if (!url_history)
        url_history = g_ptr_array_new_with_free_func(g_free);

    /* Check for duplicates */
    guint i;
    for (i = 0; i < url_history->len; i++) {
        if (strcmp(g_ptr_array_index(url_history, i), url) == 0)
            return;
    }

    g_ptr_array_add(url_history, g_strdup(url));

    /* Cap at 500 entries */
    while (url_history->len > 500)
        g_ptr_array_remove_index(url_history, 0);
}

GPtrArray *
browser_tab_get_url_history(void)
{
    return url_history;
}

void
browser_tab_set_url_history(GPtrArray *history)
{
    if (url_history)
        g_ptr_array_unref(url_history);

    if (history) {
        url_history = history;
    } else {
        url_history = NULL;
    }
}
