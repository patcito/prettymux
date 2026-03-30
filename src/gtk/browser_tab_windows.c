#define COBJMACROS

#include "browser_tab.h"

#include <ctype.h>
#include <gdk/gdkwin32.h>
#include <string.h>
#include <windows.h>
#include <WebView2.h>

typedef struct _BrowserWinBackend BrowserWinBackend;
typedef struct _BrowserEnvHandler BrowserEnvHandler;
typedef struct _BrowserControllerHandler BrowserControllerHandler;
typedef struct _BrowserSourceHandler BrowserSourceHandler;
typedef struct _BrowserTitleHandler BrowserTitleHandler;
typedef struct _BrowserNewWindowHandler BrowserNewWindowHandler;

static GPtrArray *url_history = NULL;

struct _BrowserTab {
    GtkBox parent_instance;

    GtkWidget *back_btn;
    GtkWidget *fwd_btn;
    GtkWidget *reload_btn;
    GtkWidget *url_entry;
    GtkWidget *web_host;
    BrowserWinBackend *backend;
    char *current_url;
    char *title;
    gboolean autocomplete_active;
};

struct _BrowserEnvHandler {
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler iface;
    BrowserTab *owner;
};

struct _BrowserControllerHandler {
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler iface;
    BrowserTab *owner;
};

struct _BrowserSourceHandler {
    ICoreWebView2SourceChangedEventHandler iface;
    BrowserTab *owner;
};

struct _BrowserTitleHandler {
    ICoreWebView2DocumentTitleChangedEventHandler iface;
    BrowserTab *owner;
};

struct _BrowserNewWindowHandler {
    ICoreWebView2NewWindowRequestedEventHandler iface;
    BrowserTab *owner;
};

struct _BrowserWinBackend {
    GtkWidget *host;
    guint tick_id;
    HMODULE loader_dll;
    HWND container_hwnd;
    gboolean create_started;
    gboolean com_initialized;
    ICoreWebView2Controller *controller;
    ICoreWebView2 *web_view;
    EventRegistrationToken source_token;
    EventRegistrationToken title_token;
    EventRegistrationToken new_window_token;
    BrowserEnvHandler env_handler;
    BrowserControllerHandler controller_handler;
    BrowserSourceHandler source_handler;
    BrowserTitleHandler title_handler;
    BrowserNewWindowHandler new_window_handler;
};

typedef HRESULT (STDAPICALLTYPE *CreateCoreWebView2EnvironmentWithOptionsFn)(
    PCWSTR browserExecutableFolder,
    PCWSTR userDataFolder,
    ICoreWebView2EnvironmentOptions *environmentOptions,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *environmentCreatedHandler);

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
    BrowserWinBackend *backend = self->backend;
    LPWSTR title = NULL;
    LPWSTR uri = NULL;

    if (backend == NULL || backend->web_view == NULL)
        return;

    if (SUCCEEDED(ICoreWebView2_get_Source(backend->web_view, &uri)) && uri != NULL) {
        g_autofree char *utf8 = g_utf16_to_utf8((const gunichar2 *)uri, -1, NULL, NULL, NULL);
        browser_tab_set_current_url(self, utf8);
        CoTaskMemFree(uri);
    }

    if (SUCCEEDED(ICoreWebView2_get_DocumentTitle(backend->web_view, &title)) && title != NULL) {
        g_autofree char *utf8 = g_utf16_to_utf8((const gunichar2 *)title, -1, NULL, NULL, NULL);
        browser_tab_set_title(self, utf8);
        CoTaskMemFree(title);
    }
}

static void
browser_tab_load_native(BrowserTab *self, const char *url)
{
    BrowserWinBackend *backend = self->backend;
    gunichar2 *wide;

    if (backend == NULL || backend->web_view == NULL || url == NULL || url[0] == '\0')
        return;

    wide = g_utf8_to_utf16(url, -1, NULL, NULL, NULL);
    if (wide == NULL)
        return;

    ICoreWebView2_Navigate(backend->web_view, (LPCWSTR)wide);
    g_free(wide);
}

static HRESULT STDMETHODCALLTYPE
browser_com_query_interface(void *self, REFIID riid, void **ppv, const IID *iid)
{
    if (ppv == NULL)
        return E_POINTER;

    *ppv = NULL;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, iid)) {
        *ppv = self;
        return S_OK;
    }

    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE
browser_com_addref(void *self G_GNUC_UNUSED)
{
    return 1;
}

static ULONG STDMETHODCALLTYPE
browser_com_release(void *self G_GNUC_UNUSED)
{
    return 1;
}

static HRESULT STDMETHODCALLTYPE
browser_env_query_interface(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *self,
                            REFIID riid,
                            void **ppv)
{
    return browser_com_query_interface(self, riid, ppv,
                                       &IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler);
}

static ULONG STDMETHODCALLTYPE
browser_env_addref(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *self)
{
    return browser_com_addref(self);
}

static ULONG STDMETHODCALLTYPE
browser_env_release(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *self)
{
    return browser_com_release(self);
}

static HRESULT STDMETHODCALLTYPE
browser_controller_query_interface(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *self,
                                   REFIID riid,
                                   void **ppv)
{
    return browser_com_query_interface(self, riid, ppv,
                                       &IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler);
}

static ULONG STDMETHODCALLTYPE
browser_controller_addref(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *self)
{
    return browser_com_addref(self);
}

static ULONG STDMETHODCALLTYPE
browser_controller_release(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *self)
{
    return browser_com_release(self);
}

static HRESULT STDMETHODCALLTYPE
browser_source_query_interface(ICoreWebView2SourceChangedEventHandler *self,
                               REFIID riid,
                               void **ppv)
{
    return browser_com_query_interface(self, riid, ppv, &IID_ICoreWebView2SourceChangedEventHandler);
}

static ULONG STDMETHODCALLTYPE
browser_source_addref(ICoreWebView2SourceChangedEventHandler *self)
{
    return browser_com_addref(self);
}

static ULONG STDMETHODCALLTYPE
browser_source_release(ICoreWebView2SourceChangedEventHandler *self)
{
    return browser_com_release(self);
}

static HRESULT STDMETHODCALLTYPE
browser_title_query_interface(ICoreWebView2DocumentTitleChangedEventHandler *self,
                              REFIID riid,
                              void **ppv)
{
    return browser_com_query_interface(self, riid, ppv,
                                       &IID_ICoreWebView2DocumentTitleChangedEventHandler);
}

static ULONG STDMETHODCALLTYPE
browser_title_addref(ICoreWebView2DocumentTitleChangedEventHandler *self)
{
    return browser_com_addref(self);
}

static ULONG STDMETHODCALLTYPE
browser_title_release(ICoreWebView2DocumentTitleChangedEventHandler *self)
{
    return browser_com_release(self);
}

static HRESULT STDMETHODCALLTYPE
browser_new_window_query_interface(ICoreWebView2NewWindowRequestedEventHandler *self,
                                   REFIID riid,
                                   void **ppv)
{
    return browser_com_query_interface(self, riid, ppv,
                                       &IID_ICoreWebView2NewWindowRequestedEventHandler);
}

static ULONG STDMETHODCALLTYPE
browser_new_window_addref(ICoreWebView2NewWindowRequestedEventHandler *self)
{
    return browser_com_addref(self);
}

static ULONG STDMETHODCALLTYPE
browser_new_window_release(ICoreWebView2NewWindowRequestedEventHandler *self)
{
    return browser_com_release(self);
}

static HRESULT STDMETHODCALLTYPE
browser_source_invoke(ICoreWebView2SourceChangedEventHandler *self,
                      ICoreWebView2 *sender G_GNUC_UNUSED,
                      ICoreWebView2SourceChangedEventArgs *args G_GNUC_UNUSED)
{
    BrowserSourceHandler *handler = (BrowserSourceHandler *)self;
    browser_tab_sync_from_native(handler->owner);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE
browser_title_invoke(ICoreWebView2DocumentTitleChangedEventHandler *self,
                     ICoreWebView2 *sender G_GNUC_UNUSED,
                     IUnknown *args G_GNUC_UNUSED)
{
    BrowserTitleHandler *handler = (BrowserTitleHandler *)self;
    browser_tab_sync_from_native(handler->owner);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE
browser_new_window_invoke(ICoreWebView2NewWindowRequestedEventHandler *self,
                          ICoreWebView2 *sender G_GNUC_UNUSED,
                          ICoreWebView2NewWindowRequestedEventArgs *args)
{
    BrowserNewWindowHandler *handler = (BrowserNewWindowHandler *)self;
    LPWSTR uri = NULL;

    if (SUCCEEDED(ICoreWebView2NewWindowRequestedEventArgs_get_Uri(args, &uri)) && uri != NULL) {
        g_autofree char *utf8 = g_utf16_to_utf8((const gunichar2 *)uri, -1, NULL, NULL, NULL);
        if (utf8 != NULL)
            g_signal_emit(handler->owner, signals[SIGNAL_NEW_TAB_REQUESTED], 0, utf8);
        CoTaskMemFree(uri);
    }

    ICoreWebView2NewWindowRequestedEventArgs_put_Handled(args, TRUE);
    return S_OK;
}

static const ICoreWebView2SourceChangedEventHandlerVtbl browser_source_vtbl = {
    browser_source_query_interface,
    browser_source_addref,
    browser_source_release,
    browser_source_invoke,
};

static const ICoreWebView2DocumentTitleChangedEventHandlerVtbl browser_title_vtbl = {
    browser_title_query_interface,
    browser_title_addref,
    browser_title_release,
    browser_title_invoke,
};

static const ICoreWebView2NewWindowRequestedEventHandlerVtbl browser_new_window_vtbl = {
    browser_new_window_query_interface,
    browser_new_window_addref,
    browser_new_window_release,
    browser_new_window_invoke,
};

static HRESULT STDMETHODCALLTYPE
browser_controller_invoke(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *self,
                          HRESULT error_code,
                          ICoreWebView2Controller *result)
{
    BrowserControllerHandler *handler = (BrowserControllerHandler *)self;
    BrowserWinBackend *backend = handler->owner->backend;
    ICoreWebView2Settings *settings = NULL;

    if (FAILED(error_code) || backend == NULL || result == NULL)
        return error_code;

    backend->controller = result;
    ICoreWebView2Controller_get_CoreWebView2(backend->controller, &backend->web_view);
    ICoreWebView2Controller_put_IsVisible(backend->controller, TRUE);

    if (backend->web_view != NULL) {
        if (SUCCEEDED(ICoreWebView2_get_Settings(backend->web_view, &settings)) && settings != NULL) {
            ICoreWebView2Settings_put_AreDevToolsEnabled(settings, TRUE);
            ICoreWebView2Settings_Release(settings);
        }

        ICoreWebView2_add_SourceChanged(backend->web_view,
                                        &backend->source_handler.iface,
                                        &backend->source_token);
        ICoreWebView2_add_DocumentTitleChanged(backend->web_view,
                                               &backend->title_handler.iface,
                                               &backend->title_token);
        ICoreWebView2_add_NewWindowRequested(backend->web_view,
                                             &backend->new_window_handler.iface,
                                             &backend->new_window_token);

        browser_tab_load_native(handler->owner, handler->owner->current_url);
        browser_tab_sync_from_native(handler->owner);
    }

    return S_OK;
}

static const ICoreWebView2CreateCoreWebView2ControllerCompletedHandlerVtbl browser_controller_vtbl = {
    browser_controller_query_interface,
    browser_controller_addref,
    browser_controller_release,
    browser_controller_invoke,
};

static HRESULT STDMETHODCALLTYPE
browser_env_invoke(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *self,
                   HRESULT error_code,
                   ICoreWebView2Environment *result)
{
    BrowserEnvHandler *handler = (BrowserEnvHandler *)self;
    BrowserWinBackend *backend = handler->owner->backend;

    if (FAILED(error_code) || backend == NULL || result == NULL || backend->container_hwnd == NULL)
        return error_code;

    return ICoreWebView2Environment_CreateCoreWebView2Controller(
        result,
        backend->container_hwnd,
        &backend->controller_handler.iface);
}

static const ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandlerVtbl browser_env_vtbl = {
    browser_env_query_interface,
    browser_env_addref,
    browser_env_release,
    browser_env_invoke,
};

static void
browser_tab_backend_ensure_view(BrowserTab *self)
{
    BrowserWinBackend *backend = self->backend;
    GtkNative *native;
    GdkSurface *surface;
    HWND parent_hwnd;
    CreateCoreWebView2EnvironmentWithOptionsFn create_environment;

    if (backend == NULL)
        return;

    native = gtk_widget_get_native(backend->host);
    if (native == NULL)
        return;

    surface = gtk_native_get_surface(native);
    if (surface == NULL)
        return;

    parent_hwnd = gdk_win32_surface_get_handle(surface);
    if (parent_hwnd == NULL)
        return;

    if (backend->container_hwnd == NULL) {
        backend->container_hwnd = CreateWindowExW(
            0,
            L"STATIC",
            L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
            0, 0, 1, 1,
            parent_hwnd,
            NULL,
            GetModuleHandleW(NULL),
            NULL);
    }

    if (backend->create_started || backend->container_hwnd == NULL)
        return;

    if (!backend->com_initialized) {
        if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED)))
            return;
        backend->com_initialized = TRUE;
    }

    backend->loader_dll = LoadLibraryW(L"WebView2Loader.dll");
    if (backend->loader_dll == NULL)
        return;

    create_environment =
        (CreateCoreWebView2EnvironmentWithOptionsFn)GetProcAddress(
            backend->loader_dll,
            "CreateCoreWebView2EnvironmentWithOptions");
    if (create_environment == NULL)
        return;

    backend->create_started = TRUE;
    create_environment(NULL, NULL, NULL, &backend->env_handler.iface);
}

static gboolean
browser_tab_backend_tick(GtkWidget *widget G_GNUC_UNUSED,
                         GdkFrameClock *frame_clock G_GNUC_UNUSED,
                         gpointer user_data)
{
    BrowserTab *self = BROWSER_TAB(user_data);
    BrowserWinBackend *backend = self->backend;
    GtkRoot *root;
    graphene_rect_t bounds;
    RECT rect;
    gboolean mapped;

    if (backend == NULL)
        return G_SOURCE_CONTINUE;

    browser_tab_backend_ensure_view(self);

    if (backend->container_hwnd == NULL)
        return G_SOURCE_CONTINUE;

    root = gtk_widget_get_root(backend->host);
    if (root == NULL)
        return G_SOURCE_CONTINUE;

    if (!gtk_widget_compute_bounds(backend->host, GTK_WIDGET(root), &bounds))
        return G_SOURCE_CONTINUE;

    mapped = gtk_widget_get_mapped(backend->host);
    ShowWindow(backend->container_hwnd, mapped ? SW_SHOW : SW_HIDE);
    MoveWindow(backend->container_hwnd,
               (int)bounds.origin.x,
               (int)bounds.origin.y,
               (int)bounds.size.width,
               (int)bounds.size.height,
               TRUE);

    if (backend->controller != NULL) {
        rect.left = 0;
        rect.top = 0;
        rect.right = (LONG)bounds.size.width;
        rect.bottom = (LONG)bounds.size.height;
        ICoreWebView2Controller_put_IsVisible(backend->controller, mapped ? TRUE : FALSE);
        ICoreWebView2Controller_put_Bounds(backend->controller, rect);
    }

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

        if (self->backend->web_view != NULL) {
            ICoreWebView2_remove_SourceChanged(self->backend->web_view, self->backend->source_token);
            ICoreWebView2_remove_DocumentTitleChanged(self->backend->web_view, self->backend->title_token);
            ICoreWebView2_remove_NewWindowRequested(self->backend->web_view, self->backend->new_window_token);
            ICoreWebView2_Release(self->backend->web_view);
        }

        if (self->backend->controller != NULL)
            ICoreWebView2Controller_Release(self->backend->controller);

        if (self->backend->container_hwnd != NULL)
            DestroyWindow(self->backend->container_hwnd);

        if (self->backend->loader_dll != NULL)
            FreeLibrary(self->backend->loader_dll);

        if (self->backend->com_initialized)
            CoUninitialize();

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

    self->backend = g_new0(BrowserWinBackend, 1);
    self->backend->host = self->web_host;
    self->backend->env_handler.iface.lpVtbl = &browser_env_vtbl;
    self->backend->env_handler.owner = self;
    self->backend->controller_handler.iface.lpVtbl = &browser_controller_vtbl;
    self->backend->controller_handler.owner = self;
    self->backend->source_handler.iface.lpVtbl = &browser_source_vtbl;
    self->backend->source_handler.owner = self;
    self->backend->title_handler.iface.lpVtbl = &browser_title_vtbl;
    self->backend->title_handler.owner = self;
    self->backend->new_window_handler.iface.lpVtbl = &browser_new_window_vtbl;
    self->backend->new_window_handler.owner = self;
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

    if (self->backend != NULL && self->backend->web_view != NULL)
        ICoreWebView2_GoBack(self->backend->web_view);
}

void
browser_tab_go_forward(BrowserTab *self)
{
    g_return_if_fail(BROWSER_IS_TAB(self));

    if (self->backend != NULL && self->backend->web_view != NULL)
        ICoreWebView2_GoForward(self->backend->web_view);
}

void
browser_tab_reload(BrowserTab *self)
{
    g_return_if_fail(BROWSER_IS_TAB(self));

    if (self->backend != NULL && self->backend->web_view != NULL)
        ICoreWebView2_Reload(self->backend->web_view);
}

void
browser_tab_show_inspector(BrowserTab *self)
{
    g_return_if_fail(BROWSER_IS_TAB(self));

    if (self->backend != NULL && self->backend->web_view != NULL)
        ICoreWebView2_OpenDevToolsWindow(self->backend->web_view);
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
