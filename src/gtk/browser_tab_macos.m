#include "browser_tab.h"

#include <AppKit/AppKit.h>
#include <WebKit/WebKit.h>
#include <ctype.h>
#include <gdk/macos/gdkmacos.h>
#include <string.h>

static GPtrArray *url_history = NULL;

typedef struct _BrowserMacBackend BrowserMacBackend;

struct _BrowserTab {
    GtkBox parent_instance;

    GtkWidget *back_btn;
    GtkWidget *fwd_btn;
    GtkWidget *reload_btn;
    GtkWidget *url_entry;
    GtkWidget *web_host;
    BrowserMacBackend *backend;
    char *current_url;
    char *title;
    gboolean autocomplete_active;
};

@interface PrettymuxWKDelegate : NSObject <WKNavigationDelegate, WKUIDelegate> {
@public
    BrowserTab *_owner;
}
- (instancetype)initWithOwner:(BrowserTab *)owner;
@end

struct _BrowserMacBackend {
    GtkWidget *host;
    guint tick_id;
    NSView *container_view;
    WKWebView *web_view;
    PrettymuxWKDelegate *delegate;
};

G_DEFINE_TYPE(BrowserTab, browser_tab, GTK_TYPE_BOX)

enum {
    SIGNAL_TITLE_CHANGED,
    SIGNAL_NEW_TAB_REQUESTED,
    N_SIGNALS,
};

static guint signals[N_SIGNALS];

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

static void
browser_tab_set_title(BrowserTab *self, const char *title)
{
    if (g_strcmp0(self->title, title) == 0)
        return;

    g_free(self->title);
    self->title = g_strdup(title);

    if (self->title != NULL)
        g_signal_emit(self, signals[SIGNAL_TITLE_CHANGED], 0, self->title);
}

static void
browser_tab_set_current_url(BrowserTab *self, const char *url)
{
    if (g_strcmp0(self->current_url, url) == 0)
        return;

    g_free(self->current_url);
    self->current_url = g_strdup(url);

    if (self->current_url != NULL) {
        self->autocomplete_active = TRUE;
        gtk_editable_set_text(GTK_EDITABLE(self->url_entry), self->current_url);
        self->autocomplete_active = FALSE;
        browser_tab_add_url_to_history(self->current_url);
    }
}

static void
browser_tab_sync_from_native(BrowserTab *self)
{
    BrowserMacBackend *backend = self->backend;
    if (backend == NULL || backend->web_view == nil)
        return;

    NSURL *url = backend->web_view.URL;
    NSString *title = backend->web_view.title;

    browser_tab_set_current_url(self, url != nil ? url.absoluteString.UTF8String : NULL);
    browser_tab_set_title(self, title != nil ? title.UTF8String : NULL);
}

static void
browser_tab_load_native(BrowserTab *self, const char *url)
{
    BrowserMacBackend *backend = self->backend;
    if (backend == NULL || backend->web_view == nil || url == NULL || url[0] == '\0')
        return;

    NSString *ns_url = [NSString stringWithUTF8String:url];
    NSURL *resolved = ns_url != nil ? [NSURL URLWithString:ns_url] : nil;
    if (resolved == nil)
        return;

    NSURLRequest *request = [NSURLRequest requestWithURL:resolved];
    [backend->web_view loadRequest:request];
}

static void
browser_tab_backend_ensure_view(BrowserTab *self)
{
    BrowserMacBackend *backend = self->backend;
    GtkNative *native;
    GdkSurface *surface;
    NSWindow *window;
    NSView *content_view;

    if (backend == NULL || backend->web_view != nil)
        return;

    native = gtk_widget_get_native(backend->host);
    if (native == NULL)
        return;

    surface = gtk_native_get_surface(native);
    if (surface == NULL || !GDK_IS_MACOS_SURFACE(surface))
        return;

    window = (NSWindow *)gdk_macos_surface_get_native_window(GDK_MACOS_SURFACE(surface));
    if (window == nil)
        return;

    content_view = [window contentView];
    if (content_view == nil)
        return;

    backend->container_view = [[NSView alloc] initWithFrame:NSZeroRect];
    backend->web_view = [[WKWebView alloc] initWithFrame:NSZeroRect];
    backend->delegate = [[PrettymuxWKDelegate alloc] initWithOwner:self];

    backend->web_view.navigationDelegate = backend->delegate;
    backend->web_view.UIDelegate = backend->delegate;
    [backend->web_view setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [backend->container_view addSubview:backend->web_view];
    [content_view addSubview:backend->container_view];

    if (self->current_url != NULL)
        browser_tab_load_native(self, self->current_url);
}

static gboolean
browser_tab_backend_tick(GtkWidget *widget G_GNUC_UNUSED,
                         GdkFrameClock *frame_clock G_GNUC_UNUSED,
                         gpointer user_data)
{
    BrowserTab *self = BROWSER_TAB(user_data);
    BrowserMacBackend *backend = self->backend;
    GtkRoot *root;
    graphene_rect_t bounds;
    GtkNative *native;
    GdkSurface *surface;
    NSWindow *window;
    NSView *content_view;
    CGFloat flipped_y;

    if (backend == NULL)
        return G_SOURCE_CONTINUE;

    browser_tab_backend_ensure_view(self);

    if (backend->container_view == nil)
        return G_SOURCE_CONTINUE;

    root = gtk_widget_get_root(backend->host);
    if (root == NULL)
        return G_SOURCE_CONTINUE;

    native = gtk_widget_get_native(backend->host);
    if (native == NULL)
        return G_SOURCE_CONTINUE;

    surface = gtk_native_get_surface(native);
    if (surface == NULL || !GDK_IS_MACOS_SURFACE(surface))
        return G_SOURCE_CONTINUE;

    window = (NSWindow *)gdk_macos_surface_get_native_window(GDK_MACOS_SURFACE(surface));
    if (window == nil)
        return G_SOURCE_CONTINUE;

    content_view = [window contentView];
    if (content_view == nil)
        return G_SOURCE_CONTINUE;

    if (!gtk_widget_compute_bounds(backend->host, GTK_WIDGET(root), &bounds))
        return G_SOURCE_CONTINUE;

    flipped_y = content_view.bounds.size.height - bounds.origin.y - bounds.size.height;
    [backend->container_view setHidden:!gtk_widget_get_mapped(backend->host)];
    [backend->container_view setFrame:NSMakeRect(bounds.origin.x,
                                                 flipped_y,
                                                 bounds.size.width,
                                                 bounds.size.height)];
    [backend->web_view setFrame:backend->container_view.bounds];
    return G_SOURCE_CONTINUE;
}

static void
on_back_clicked(GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
    browser_tab_go_back(BROWSER_TAB(user_data));
}

static void
on_fwd_clicked(GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
    browser_tab_go_forward(BROWSER_TAB(user_data));
}

static void
on_reload_clicked(GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
    browser_tab_reload(BROWSER_TAB(user_data));
}

static void
on_url_entry_activate(GtkEntry *entry G_GNUC_UNUSED, gpointer user_data)
{
    BrowserTab *self = BROWSER_TAB(user_data);
    const char *text = gtk_editable_get_text(GTK_EDITABLE(self->url_entry));
    g_autofree char *trimmed = g_strstrip(g_strdup(text));

    if (trimmed[0] == '\0')
        return;

    browser_tab_navigate(self, trimmed);
}

static void
on_url_entry_changed(GtkEditable *editable, gpointer user_data)
{
    BrowserTab *self = BROWSER_TAB(user_data);
    const char *text;
    int text_len;

    if (self->autocomplete_active || url_history == NULL)
        return;

    text = gtk_editable_get_text(editable);
    if (text == NULL || text[0] == '\0')
        return;

    text_len = (int)strlen(text);

    for (guint i = 0; i < url_history->len; i++) {
        const char *entry = g_ptr_array_index(url_history, i);
        if (g_ascii_strncasecmp(entry, text, text_len) == 0) {
            self->autocomplete_active = TRUE;
            gtk_editable_set_text(editable, entry);
            gtk_editable_select_region(editable, text_len, (int)strlen(entry));
            self->autocomplete_active = FALSE;
            return;
        }
    }

    for (guint i = 0; i < url_history->len; i++) {
        const char *entry = g_ptr_array_index(url_history, i);
        const char *stripped = entry;

        if (g_str_has_prefix(stripped, "https://"))
            stripped += 8;
        else if (g_str_has_prefix(stripped, "http://"))
            stripped += 7;

        if (g_ascii_strncasecmp(stripped, text, text_len) == 0) {
            self->autocomplete_active = TRUE;
            gtk_editable_set_text(editable, entry);
            gtk_editable_select_region(editable, text_len, (int)strlen(entry));
            self->autocomplete_active = FALSE;
            return;
        }
    }
}

static void
browser_tab_dispose(GObject *object)
{
    BrowserTab *self = BROWSER_TAB(object);

    if (self->backend != NULL) {
        if (self->backend->tick_id != 0)
            gtk_widget_remove_tick_callback(self->backend->host, self->backend->tick_id);

        if (self->backend->web_view != nil) {
            self->backend->web_view.navigationDelegate = nil;
            self->backend->web_view.UIDelegate = nil;
            [self->backend->web_view removeFromSuperview];
            [self->backend->web_view release];
        }

        if (self->backend->container_view != nil) {
            [self->backend->container_view removeFromSuperview];
            [self->backend->container_view release];
        }

        if (self->backend->delegate != nil)
            [self->backend->delegate release];

        g_free(self->backend);
        self->backend = NULL;
    }

    g_clear_pointer(&self->current_url, g_free);
    g_clear_pointer(&self->title, g_free);

    G_OBJECT_CLASS(browser_tab_parent_class)->dispose(object);
}

static void
browser_tab_class_init(BrowserTabClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = browser_tab_dispose;

    signals[SIGNAL_TITLE_CHANGED] =
        g_signal_new("title-changed",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_LAST,
                     0, NULL, NULL, NULL,
                     G_TYPE_NONE, 1, G_TYPE_STRING);

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
    GtkWidget *bar;

    gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
    gtk_box_set_spacing(GTK_BOX(self), 0);

    bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
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

    self->web_host = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(self->web_host, TRUE);
    gtk_widget_set_vexpand(self->web_host, TRUE);
    gtk_box_append(GTK_BOX(self), self->web_host);

    self->backend = g_new0(BrowserMacBackend, 1);
    self->backend->host = self->web_host;
    self->backend->tick_id =
        gtk_widget_add_tick_callback(self->web_host,
                                     browser_tab_backend_tick,
                                     self,
                                     NULL);

    g_signal_connect(self->back_btn, "clicked", G_CALLBACK(on_back_clicked), self);
    g_signal_connect(self->fwd_btn, "clicked", G_CALLBACK(on_fwd_clicked), self);
    g_signal_connect(self->reload_btn, "clicked", G_CALLBACK(on_reload_clicked), self);
    g_signal_connect(self->url_entry, "activate", G_CALLBACK(on_url_entry_activate), self);
    g_signal_connect(GTK_EDITABLE(self->url_entry), "changed",
                     G_CALLBACK(on_url_entry_changed), self);
}

GtkWidget *
browser_tab_new(const char *url)
{
    BrowserTab *self = g_object_new(BROWSER_TYPE_TAB, NULL);

    if (url != NULL && url[0] != '\0')
        browser_tab_navigate(self, url);

    return GTK_WIDGET(self);
}

const char *
browser_tab_get_url(BrowserTab *self)
{
    g_return_val_if_fail(BROWSER_IS_TAB(self), NULL);
    return self->current_url;
}

const char *
browser_tab_get_title(BrowserTab *self)
{
    g_return_val_if_fail(BROWSER_IS_TAB(self), NULL);
    return self->title;
}

void
browser_tab_navigate(BrowserTab *self, const char *url)
{
    g_autofree char *resolved = NULL;

    g_return_if_fail(BROWSER_IS_TAB(self));
    g_return_if_fail(url != NULL);

    resolved = url_from_input(url);
    browser_tab_set_current_url(self, resolved);
    browser_tab_load_native(self, resolved);
}

void
browser_tab_go_back(BrowserTab *self)
{
    g_return_if_fail(BROWSER_IS_TAB(self));

    if (self->backend != NULL && self->backend->web_view != nil)
        [self->backend->web_view goBack];
}

void
browser_tab_go_forward(BrowserTab *self)
{
    g_return_if_fail(BROWSER_IS_TAB(self));

    if (self->backend != NULL && self->backend->web_view != nil)
        [self->backend->web_view goForward];
}

void
browser_tab_reload(BrowserTab *self)
{
    g_return_if_fail(BROWSER_IS_TAB(self));

    if (self->backend != NULL && self->backend->web_view != nil)
        [self->backend->web_view reload];
}

void
browser_tab_show_inspector(BrowserTab *self G_GNUC_UNUSED)
{
}

void
browser_tab_focus_url(BrowserTab *self)
{
    g_return_if_fail(BROWSER_IS_TAB(self));
    gtk_widget_grab_focus(self->url_entry);
    gtk_editable_select_region(GTK_EDITABLE(self->url_entry), 0, -1);
}

void
browser_tab_add_url_to_history(const char *url)
{
    if (url == NULL || url[0] == '\0')
        return;
    if (g_str_has_prefix(url, "about:") || g_str_has_prefix(url, "data:"))
        return;

    if (url_history == NULL)
        url_history = g_ptr_array_new_with_free_func(g_free);

    for (guint i = 0; i < url_history->len; i++) {
        if (strcmp(g_ptr_array_index(url_history, i), url) == 0)
            return;
    }

    g_ptr_array_add(url_history, g_strdup(url));

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
    if (url_history != NULL)
        g_ptr_array_unref(url_history);

    url_history = history;
}

@implementation PrettymuxWKDelegate

- (instancetype)initWithOwner:(BrowserTab *)owner
{
    self = [super init];
    if (self != nil)
        _owner = owner;
    return self;
}

- (void)webView:(WKWebView *)webView didStartProvisionalNavigation:(WKNavigation *)navigation
{
    (void)webView;
    (void)navigation;
    browser_tab_sync_from_native(_owner);
}

- (void)webView:(WKWebView *)webView didCommitNavigation:(WKNavigation *)navigation
{
    (void)webView;
    (void)navigation;
    browser_tab_sync_from_native(_owner);
}

- (void)webView:(WKWebView *)webView didFinishNavigation:(WKNavigation *)navigation
{
    (void)webView;
    (void)navigation;
    browser_tab_sync_from_native(_owner);
}

- (WKWebView *)webView:(WKWebView *)webView
createWebViewWithConfiguration:(WKWebViewConfiguration *)configuration
    forNavigationAction:(WKNavigationAction *)navigationAction
         windowFeatures:(WKWindowFeatures *)windowFeatures
{
    NSURL *url;

    (void)webView;
    (void)configuration;
    (void)windowFeatures;

    url = navigationAction.request.URL;
    if (url != nil && url.absoluteString != nil)
        g_signal_emit(_owner, signals[SIGNAL_NEW_TAB_REQUESTED], 0, url.absoluteString.UTF8String);
    return nil;
}

@end
