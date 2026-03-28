#include "browser_tab.h"

#include <string.h>
#include <ctype.h>

/* ---------- type definition ---------- */

struct _BrowserTab {
    GtkBox parent_instance;

    GtkWidget *back_btn;
    GtkWidget *fwd_btn;
    GtkWidget *reload_btn;
    GtkWidget *url_entry;
    WebKitWebView *web_view;
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
    if (uri != NULL)
        gtk_editable_set_text(GTK_EDITABLE(self->url_entry), uri);
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
    gtk_box_append(GTK_BOX(self), GTK_WIDGET(self->web_view));

    /* --- connect signals --- */
    g_signal_connect(self->back_btn, "clicked",
                     G_CALLBACK(on_back_clicked), self);
    g_signal_connect(self->fwd_btn, "clicked",
                     G_CALLBACK(on_fwd_clicked), self);
    g_signal_connect(self->reload_btn, "clicked",
                     G_CALLBACK(on_reload_clicked), self);
    g_signal_connect(self->url_entry, "activate",
                     G_CALLBACK(on_url_entry_activate), self);

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
    webkit_web_inspector_show(inspector);
}
