// PrettyMux — GTK4 + WebKitGTK + ghostty (OpenGL)
// GPU-accelerated terminal multiplexer with integrated browser

#include <adwaita.h>
#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>

#include "ghostty.h"
#include "ghostty_terminal.h"
#include "browser_tab.h"
#include "theme.h"
#include "shortcuts.h"
#include "workspace.h"
#include "session.h"

// ── Global state ──

ghostty_app_t g_ghostty_app = NULL;
static GtkWindow *g_main_window = NULL;

// Widget references for the main window layout
static struct {
    GtkWidget *outer_paned;
    GtkWidget *sidebar_box;
    GtkWidget *workspace_list;
    GtkWidget *main_paned;
    GtkWidget *terminal_stack;
    GtkWidget *browser_notebook;
} ui = {0};

// ── Ghostty callbacks ──

static gboolean wakeup_idle(gpointer data) {
    (void)data;
    if (g_ghostty_app)
        ghostty_app_tick(g_ghostty_app);
    return G_SOURCE_REMOVE;
}

static void wakeup_cb(void *ud) {
    (void)ud;
    g_idle_add(wakeup_idle, NULL);
}
static void close_surface_cb(void *ud, _Bool rt) { (void)ud; (void)rt; }
static bool read_clipboard_cb(void *ud, ghostty_clipboard_e c, void *d) {
    (void)ud; (void)c; (void)d; return false;
}
static void confirm_read_clipboard_cb(void *ud, const char *t, void *d, ghostty_clipboard_request_e r) {
    (void)ud; (void)t; (void)d; (void)r;
}
static void write_clipboard_cb(void *ud, ghostty_clipboard_e c, const ghostty_clipboard_content_s *co, size_t l, _Bool cf) {
    (void)ud; (void)c; (void)co; (void)l; (void)cf;
}

// ── Browser tab management ──

static void add_browser_tab(const char *url);

static void on_browser_title_changed(BrowserTab *bt, const char *title, gpointer lbl) {
    (void)bt;
    char s[24]; snprintf(s, sizeof(s), "%.20s", title);
    gtk_label_set_text(GTK_LABEL(lbl), s);
}

static void on_browser_new_tab_requested(BrowserTab *bt, const char *url, gpointer d) {
    (void)bt; (void)d;
    add_browser_tab(url);
}

static void add_browser_tab(const char *url) {
    GtkWidget *tab = browser_tab_new(url);
    GtkWidget *label = gtk_label_new("Loading...");

    g_signal_connect(tab, "title-changed", G_CALLBACK(on_browser_title_changed), label);
    g_signal_connect(tab, "new-tab-requested", G_CALLBACK(on_browser_new_tab_requested), NULL);

    int idx = gtk_notebook_append_page(GTK_NOTEBOOK(ui.browser_notebook), tab, label);
    gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(ui.browser_notebook), tab, TRUE);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(ui.browser_notebook), idx);
    gtk_widget_set_visible(tab, TRUE);
}

// ── Action dispatch ──

static void handle_action(const char *action) {
    if (strcmp(action, "workspace.new") == 0) {
        workspace_add(ui.terminal_stack, ui.workspace_list, g_ghostty_app);
    } else if (strcmp(action, "workspace.close") == 0) {
        workspace_remove(current_workspace, ui.terminal_stack, ui.workspace_list);
    } else if (strcmp(action, "workspace.next") == 0) {
        workspace_switch((current_workspace + 1) % workspaces->len,
                         ui.terminal_stack, ui.workspace_list);
    } else if (strcmp(action, "workspace.prev") == 0) {
        workspace_switch((current_workspace - 1 + workspaces->len) % workspaces->len,
                         ui.terminal_stack, ui.workspace_list);
    } else if (strcmp(action, "pane.tab.new") == 0) {
        Workspace *ws = workspace_get_current();
        if (ws) workspace_add_terminal(ws, g_ghostty_app);
    } else if (strcmp(action, "browser.toggle") == 0) {
        gboolean vis = gtk_widget_get_visible(ui.browser_notebook);
        gtk_widget_set_visible(ui.browser_notebook, !vis);
    } else if (strcmp(action, "browser.new") == 0) {
        add_browser_tab("https://prettymux-web.vercel.app/?prettymux=t");
        gtk_widget_set_visible(ui.browser_notebook, TRUE);
    } else if (strcmp(action, "devtools.docked") == 0 || strcmp(action, "devtools.window") == 0) {
        int pg = gtk_notebook_get_current_page(GTK_NOTEBOOK(ui.browser_notebook));
        if (pg >= 0) {
            GtkWidget *child = gtk_notebook_get_nth_page(GTK_NOTEBOOK(ui.browser_notebook), pg);
            if (BROWSER_IS_TAB(child)) browser_tab_show_inspector(BROWSER_TAB(child));
        }
    } else if (strcmp(action, "theme.cycle") == 0) {
        theme_cycle();
    } else if (strcmp(action, "pane.close") == 0) {
        Workspace *ws = workspace_get_current();
        if (ws) {
            int pg = gtk_notebook_get_current_page(GTK_NOTEBOOK(ws->notebook));
            if (pg >= 0 && gtk_notebook_get_n_pages(GTK_NOTEBOOK(ws->notebook)) > 1)
                gtk_notebook_remove_page(GTK_NOTEBOOK(ws->notebook), pg);
        }
    } else if (strcmp(action, "broadcast.toggle") == 0) {
        Workspace *ws = workspace_get_current();
        if (ws) ws->broadcast = !ws->broadcast;
    }
}

// ── Keyboard handler (capture phase) ──

static gboolean on_key_pressed(GtkEventControllerKey *ctrl, guint keyval,
                                guint keycode, GdkModifierType state, gpointer d)
{
    (void)ctrl; (void)keycode; (void)d;
    const char *action = shortcut_match(keyval, state);
    if (action) { handle_action(action); return TRUE; }
    return FALSE;
}

// ── Ghostty action callback ──

static bool action_cb(ghostty_app_t app, ghostty_target_s target, ghostty_action_s action) {
    (void)app; (void)target;
    switch (action.tag) {
        case GHOSTTY_ACTION_OPEN_URL:
            if (action.action.open_url.url) {
                add_browser_tab(action.action.open_url.url);
                gtk_widget_set_visible(ui.browser_notebook, TRUE);
            }
            break;
        case GHOSTTY_ACTION_DESKTOP_NOTIFICATION: {
            GNotification *n = g_notification_new("PrettyMux");
            g_notification_set_body(n, action.action.desktop_notification.body);
            GApplication *a = g_application_get_default();
            if (a) g_application_send_notification(a, NULL, n);
            g_object_unref(n);
            break;
        }
        default: break;
    }
    return true;
}

// ── Sidebar ──

static void on_workspace_row_activated(GtkListBox *list, GtkListBoxRow *row, gpointer d) {
    (void)list; (void)d;
    workspace_switch(gtk_list_box_row_get_index(row), ui.terminal_stack, ui.workspace_list);
}

// ── Build window ──

static void on_add_workspace_clicked(GtkButton *b, gpointer d) {
    (void)b; (void)d;
    workspace_add(ui.terminal_stack, ui.workspace_list, g_ghostty_app);
}

static void build_sidebar(void) {
    ui.sidebar_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(ui.sidebar_box, "sidebar");
    gtk_widget_set_size_request(ui.sidebar_box, 180, -1);

    // Search
    GtkWidget *search = gtk_search_entry_new();
    gtk_widget_set_margin_start(search, 8);
    gtk_widget_set_margin_end(search, 8);
    gtk_widget_set_margin_top(search, 8);
    gtk_widget_set_margin_bottom(search, 4);
    gtk_box_append(GTK_BOX(ui.sidebar_box), search);

    // Workspace list
    ui.workspace_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(ui.workspace_list), GTK_SELECTION_SINGLE);
    g_signal_connect(ui.workspace_list, "row-activated", G_CALLBACK(on_workspace_row_activated), NULL);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), ui.workspace_list);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(ui.sidebar_box), scroll);

    // Add workspace button
    GtkWidget *btn = gtk_button_new_with_label("+ New Workspace");
    gtk_widget_set_margin_start(btn, 8);
    gtk_widget_set_margin_end(btn, 8);
    gtk_widget_set_margin_bottom(btn, 8);
    gtk_widget_set_margin_top(btn, 4);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_add_workspace_clicked), NULL);
    gtk_box_append(GTK_BOX(ui.sidebar_box), btn);
}

static void on_new_browser_tab_clicked(GtkButton *b, gpointer d) {
    (void)b; (void)d;
    add_browser_tab("https://prettymux-web.vercel.app/?prettymux=t");
}

static void build_browser(void) {
    ui.browser_notebook = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(ui.browser_notebook), TRUE);
    gtk_notebook_set_show_border(GTK_NOTEBOOK(ui.browser_notebook), FALSE);

    GtkWidget *btn = gtk_button_new_with_label("+");
    g_signal_connect(btn, "clicked", G_CALLBACK(on_new_browser_tab_clicked), NULL);
    gtk_notebook_set_action_widget(GTK_NOTEBOOK(ui.browser_notebook), btn, GTK_PACK_END);
    gtk_widget_set_visible(btn, TRUE);
}

static gboolean autosave_tick(gpointer d) {
    (void)d;
    session_save(g_main_window, ui.browser_notebook, ui.terminal_stack, ui.workspace_list);
    return G_SOURCE_CONTINUE;
}

static gboolean on_close_request(GtkWindow *w, gpointer d) {
    (void)d;
    session_save(w, ui.browser_notebook, ui.terminal_stack, ui.workspace_list);
    return FALSE;
}

static void on_activate(GtkApplication *app, gpointer user_data) {
    (void)user_data;

    // Init ghostty
    if (ghostty_init(0, NULL) != 0) { fprintf(stderr, "ghostty_init failed\n"); return; }

    ghostty_config_t config = ghostty_config_new();
    ghostty_config_load_default_files(config);
    ghostty_config_finalize(config);

    ghostty_runtime_config_s rc = {0};
    rc.wakeup_cb = wakeup_cb;
    rc.action_cb = action_cb;
    rc.read_clipboard_cb = read_clipboard_cb;
    rc.confirm_read_clipboard_cb = confirm_read_clipboard_cb;
    rc.write_clipboard_cb = write_clipboard_cb;
    rc.close_surface_cb = close_surface_cb;

    g_ghostty_app = ghostty_app_new(&rc, config);
    if (!g_ghostty_app) { fprintf(stderr, "ghostty_app_new failed\n"); return; }

    // Theme
    theme_apply();

    // Window
    GtkWidget *window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "PrettyMux");
    gtk_window_set_default_size(GTK_WINDOW(window), 1400, 900);
    g_main_window = GTK_WINDOW(window);

    // Shortcut handler (capture phase)
    GtkEventController *kc = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(kc, GTK_PHASE_CAPTURE);
    g_signal_connect(kc, "key-pressed", G_CALLBACK(on_key_pressed), NULL);
    gtk_widget_add_controller(window, kc);

    // Layout
    ui.outer_paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_window_set_child(GTK_WINDOW(window), ui.outer_paned);

    build_sidebar();
    gtk_paned_set_start_child(GTK_PANED(ui.outer_paned), ui.sidebar_box);
    gtk_paned_set_resize_start_child(GTK_PANED(ui.outer_paned), FALSE);
    gtk_paned_set_shrink_start_child(GTK_PANED(ui.outer_paned), FALSE);

    ui.main_paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_end_child(GTK_PANED(ui.outer_paned), ui.main_paned);

    ui.terminal_stack = gtk_stack_new();
    gtk_widget_set_hexpand(ui.terminal_stack, TRUE);
    gtk_widget_set_vexpand(ui.terminal_stack, TRUE);
    gtk_paned_set_start_child(GTK_PANED(ui.main_paned), ui.terminal_stack);

    build_browser();
    gtk_paned_set_end_child(GTK_PANED(ui.main_paned), ui.browser_notebook);

    gtk_paned_set_position(GTK_PANED(ui.outer_paned), 200);
    gtk_paned_set_position(GTK_PANED(ui.main_paned), 700);

    // Create initial workspace + restore or create defaults
    workspace_add(ui.terminal_stack, ui.workspace_list, g_ghostty_app);

    if (session_exists()) {
        session_restore(GTK_WINDOW(window), ui.browser_notebook,
                        ui.terminal_stack, ui.workspace_list, g_ghostty_app);
    }
    // Always ensure at least one browser tab exists
    if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(ui.browser_notebook)) == 0) {
        add_browser_tab("https://prettymux-web.vercel.app/?prettymux=t");
    }

    // Auto-save
    g_timeout_add_seconds(30, autosave_tick, NULL);

    // Save on close
    g_signal_connect(window, "close-request", G_CALLBACK(on_close_request), NULL);

    gtk_window_present(GTK_WINDOW(window));
}

// ── Entry point ──

int main(int argc, char *argv[]) {
    // WebKitGTK's bubblewrap sandbox requires dbus-proxy which may not
    // be available in all environments. Disable it for now.
    g_setenv("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS", "1", FALSE);

    AdwApplication *app = adw_application_new(NULL, G_APPLICATION_FLAGS_NONE);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    if (g_ghostty_app) ghostty_app_free(g_ghostty_app);
    return status;
}
