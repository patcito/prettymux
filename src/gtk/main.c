// PrettyMux — GTK4 + WebKitGTK + ghostty (OpenGL)
// GPU-accelerated terminal multiplexer with integrated browser

#include <adwaita.h>
#include <gtk/gtk.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <libgen.h>

#include "ghostty.h"
#include "ghostty_terminal.h"
#include "browser_tab.h"
#include "theme.h"
#include "shortcuts.h"
#include "workspace.h"
#include "session.h"
#include "command_palette.h"
#include "port_scanner.h"
#include "socket_server.h"
#include "shortcuts_overlay.h"
#include "pip_window.h"

// ── Global state ──

ghostty_app_t g_ghostty_app = NULL;
static GtkWindow *g_main_window = NULL;

// Widget references for the main window layout
static struct {
    GtkWidget *outer_paned;
    GtkWidget *sidebar_box;
    GtkWidget *workspace_list;
    GtkWidget *main_paned;
    GtkWidget *terminal_box;      // GtkBox holding terminal_stack + notes panel
    GtkWidget *terminal_stack;
    GtkWidget *browser_notebook;
    GtkWidget *overlay;           // GtkOverlay wrapping the outer_paned
    GtkWidget *command_palette;   // CommandPalette overlay widget
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
        if (ws) {
            GtkNotebook *focused = workspace_get_focused_pane(ws);
            if (focused) {
                /* Declared in workspace.c as static; use the public API
                 * which adds to the first notebook.  For focused-pane
                 * support, we call workspace_add_terminal which now goes
                 * to the focused pane. */
                workspace_add_terminal_to_focused(ws, g_ghostty_app);
            } else {
                workspace_add_terminal(ws, g_ghostty_app);
            }
        }
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
            GtkNotebook *focused = workspace_get_focused_pane(ws);
            if (focused) {
                int n_pages = gtk_notebook_get_n_pages(focused);
                int pg = gtk_notebook_get_current_page(focused);
                if (n_pages > 1 && pg >= 0) {
                    /* Close the current tab in this pane */
                    GtkWidget *child = gtk_notebook_get_nth_page(focused, pg);
                    g_ptr_array_remove(ws->terminals, child);
                    gtk_notebook_remove_page(focused, pg);
                } else if (n_pages <= 1 && ws->pane_notebooks &&
                           ws->pane_notebooks->len > 1) {
                    /* Last tab in this pane — close the entire pane */
                    workspace_close_pane(ws, focused);
                }
            }
        }
    } else if (strcmp(action, "broadcast.toggle") == 0) {
        Workspace *ws = workspace_get_current();
        if (ws) ws->broadcast = !ws->broadcast;
    } else if (strcmp(action, "split.horizontal") == 0) {
        Workspace *ws = workspace_get_current();
        if (ws) workspace_split_pane(ws, GTK_ORIENTATION_HORIZONTAL, g_ghostty_app);
    } else if (strcmp(action, "split.vertical") == 0) {
        Workspace *ws = workspace_get_current();
        if (ws) workspace_split_pane(ws, GTK_ORIENTATION_VERTICAL, g_ghostty_app);
    } else if (strcmp(action, "search.show") == 0) {
        if (ui.command_palette)
            command_palette_toggle(COMMAND_PALETTE(ui.command_palette));
    } else if (strcmp(action, "shortcuts.show") == 0) {
        /* Feature 1: Shortcuts overlay */
        if (ui.overlay)
            shortcuts_overlay_toggle(GTK_OVERLAY(ui.overlay));
    } else if (strcmp(action, "pip.toggle") == 0) {
        /* Feature 2: Picture-in-Picture window */
        pip_window_toggle(g_main_window, ui.browser_notebook);
    } else if (strcmp(action, "pane.zoom") == 0) {
        /* Feature 3: Pane zoom */
        Workspace *ws = workspace_get_current();
        if (ws) workspace_toggle_zoom(ws);
    } else if (strcmp(action, "notes.toggle") == 0) {
        /* Feature 4: Notes panel */
        Workspace *ws = workspace_get_current();
        if (ws && ui.terminal_box)
            workspace_toggle_notes(ws, ui.terminal_box);
    } else if (strcmp(action, "terminal.search") == 0) {
        /* Feature 5: Terminal search via ghostty built-in */
        Workspace *ws = workspace_get_current();
        if (ws) {
            GtkNotebook *focused = workspace_get_focused_pane(ws);
            if (focused) {
                int pg = gtk_notebook_get_current_page(focused);
                if (pg >= 0) {
                    GtkWidget *child = gtk_notebook_get_nth_page(focused, pg);
                    if (child && GHOSTTY_IS_TERMINAL(child)) {
                        ghostty_surface_t surface =
                            ghostty_terminal_get_surface(GHOSTTY_TERMINAL(child));
                        if (surface)
                            ghostty_surface_binding_action(surface,
                                                           "search_forward", 14);
                    }
                }
            }
        }
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

// ── Terminal lookup: find GhosttyTerminal by ghostty_surface_t ──

typedef struct {
    GhosttyTerminal *terminal;
    Workspace       *workspace;
    int              workspace_idx;
} SurfaceLookup;

static SurfaceLookup
find_terminal_for_surface(ghostty_surface_t surface)
{
    SurfaceLookup result = { NULL, NULL, -1 };
    if (!surface || !workspaces)
        return result;

    for (guint wi = 0; wi < workspaces->len; wi++) {
        Workspace *ws = g_ptr_array_index(workspaces, wi);
        for (guint ti = 0; ti < ws->terminals->len; ti++) {
            GhosttyTerminal *term = g_ptr_array_index(ws->terminals, ti);
            if (ghostty_terminal_get_surface(term) == surface) {
                result.terminal = term;
                result.workspace = ws;
                result.workspace_idx = (int)wi;
                return result;
            }
        }
    }
    return result;
}

// ── Port scanner callback ──

static void
on_new_ports_detected(const int *new_ports, int new_port_count,
                      const int *all_ports, int all_port_count,
                      gpointer user_data)
{
    (void)all_ports;
    (void)all_port_count;
    (void)user_data;

    if (new_port_count <= 0)
        return;

    /* Update current workspace notification */
    Workspace *ws = workspace_get_current();
    if (!ws)
        return;

    /* Only show ports if the workspace has a running command
     * (title is not just a shell name or path) */
    if (ws->name[0] == '\0' ||
        strchr(ws->name, '/') != NULL ||
        strcmp(ws->name, "bash") == 0 ||
        strcmp(ws->name, "zsh") == 0 ||
        strcmp(ws->name, "fish") == 0 ||
        strcmp(ws->name, "sh") == 0)
        return;

    int port = new_ports[0];
    snprintf(ws->notification, sizeof(ws->notification),
             "-> localhost:%d", port);

    /* Desktop notification */
    char msg[64];
    snprintf(msg, sizeof(msg), "Port %d detected", port);

    GError *nerr = NULL;
    GSubprocess *nproc = g_subprocess_new(
        G_SUBPROCESS_FLAGS_NONE, &nerr,
        "notify-send", "prettymux", msg,
        "--app-name=prettymux", NULL);
    if (nproc)
        g_object_unref(nproc);
    else if (nerr)
        g_error_free(nerr);
}

// ── Socket server callback ──

static void
on_socket_command(const char *command, const char *url, gpointer user_data)
{
    (void)user_data;

    if (strcmp(command, "browser.open") == 0 && url && url[0]) {
        add_browser_tab(url);
        gtk_widget_set_visible(ui.browser_notebook, TRUE);
    }
}

// ── Ghostty action callback ──

static bool action_cb(ghostty_app_t app, ghostty_target_s target,
                       ghostty_action_s action)
{
    (void)app;

    /* Extract surface pointer from the target */
    ghostty_surface_t surface = NULL;
    if (target.tag == GHOSTTY_TARGET_SURFACE)
        surface = target.target.surface;

    switch (action.tag) {

    case GHOSTTY_ACTION_OPEN_URL:
        if (action.action.open_url.url) {
            add_browser_tab(action.action.open_url.url);
            gtk_widget_set_visible(ui.browser_notebook, TRUE);
        }
        return true;

    case GHOSTTY_ACTION_DESKTOP_NOTIFICATION: {
        GNotification *n = g_notification_new("PrettyMux");
        g_notification_set_body(n, action.action.desktop_notification.body);
        GApplication *a = g_application_get_default();
        if (a) g_application_send_notification(a, NULL, n);
        g_object_unref(n);
        return true;
    }

    case GHOSTTY_ACTION_SET_TITLE: {
        if (!action.action.set_title.title)
            return true;
        SurfaceLookup loc = find_terminal_for_surface(surface);
        if (loc.terminal) {
            ghostty_terminal_set_title(loc.terminal,
                                       action.action.set_title.title);
            /* Also update workspace title + sidebar label */
            if (loc.workspace) {
                snprintf(loc.workspace->name, sizeof(loc.workspace->name),
                         "%.60s", action.action.set_title.title);
                workspace_refresh_sidebar_label(loc.workspace);
            }
        }
        return true;
    }

    case GHOSTTY_ACTION_PWD: {
        if (!action.action.pwd.pwd)
            return true;
        SurfaceLookup loc = find_terminal_for_surface(surface);
        if (loc.terminal) {
            ghostty_terminal_set_cwd(loc.terminal,
                                     action.action.pwd.pwd);
            if (loc.workspace) {
                snprintf(loc.workspace->cwd, sizeof(loc.workspace->cwd),
                         "%s", action.action.pwd.pwd);
                workspace_detect_git(loc.workspace);
            }
        }
        return true;
    }

    case GHOSTTY_ACTION_COMMAND_FINISHED: {
        SurfaceLookup loc = find_terminal_for_surface(surface);
        if (loc.terminal) {
            ghostty_terminal_notify_command_finished(
                loc.terminal,
                action.action.command_finished.exit_code,
                action.action.command_finished.duration);

            /* Desktop notification for long-running commands (>3s) */
            double secs = action.action.command_finished.duration / 1000000000.0;
            if (secs > 3.0) {
                char body[128];
                if (action.action.command_finished.exit_code == 0)
                    snprintf(body, sizeof(body),
                             "Command completed in %.1fs", secs);
                else
                    snprintf(body, sizeof(body),
                             "Command failed (exit %d) after %.1fs",
                             action.action.command_finished.exit_code, secs);

                GNotification *n = g_notification_new("prettymux");
                g_notification_set_body(n, body);
                GApplication *a = g_application_get_default();
                if (a) g_application_send_notification(a, NULL, n);
                g_object_unref(n);
            }
        }
        return true;
    }

    case GHOSTTY_ACTION_RING_BELL: {
        SurfaceLookup loc = find_terminal_for_surface(surface);
        if (loc.terminal)
            ghostty_terminal_notify_bell(loc.terminal);

        /* Desktop notification if not the active workspace */
        if (loc.workspace_idx >= 0 && loc.workspace_idx != current_workspace) {
            GError *bell_err = NULL;
            GSubprocess *bell_proc = g_subprocess_new(
                G_SUBPROCESS_FLAGS_NONE, &bell_err,
                "notify-send", "prettymux", "Bell",
                "--app-name=prettymux", NULL);
            if (bell_proc)
                g_object_unref(bell_proc);
            else if (bell_err)
                g_error_free(bell_err);
        }
        return true;
    }

    case GHOSTTY_ACTION_RENDER: {
        SurfaceLookup loc = find_terminal_for_surface(surface);
        if (loc.terminal)
            ghostty_terminal_queue_render(loc.terminal);
        return true;
    }

    case GHOSTTY_ACTION_SHOW_CHILD_EXITED: {
        SurfaceLookup loc = find_terminal_for_surface(surface);
        if (loc.terminal) {
            ghostty_terminal_notify_child_exited(
                loc.terminal,
                action.action.child_exited.exit_code);
        }
        return true;
    }

    case GHOSTTY_ACTION_PROGRESS_REPORT: {
        SurfaceLookup loc = find_terminal_for_surface(surface);
        if (loc.terminal && loc.workspace) {
            int pct = (int)action.action.progress_report.progress;
            if (pct >= 0)
                snprintf(loc.workspace->notification,
                         sizeof(loc.workspace->notification),
                         "Progress: %d%%", pct);
            else
                loc.workspace->notification[0] = '\0';
        }
        return true;
    }

    case GHOSTTY_ACTION_START_SEARCH:
    case GHOSTTY_ACTION_SEARCH_TOTAL:
    case GHOSTTY_ACTION_SEARCH_SELECTED:
        return true;

    default:
        break;
    }

    return false;
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
    port_scanner_stop();
    socket_server_stop();
    return FALSE;
}

// ── Feature 3: Welcome Dialog ──

static void
on_welcome_ok_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    AdwDialog *dlg = ADW_DIALOG(user_data);

    /* Check if the "don't show again" checkbox is checked */
    GtkWidget *check = g_object_get_data(G_OBJECT(dlg), "check-button");
    if (check && gtk_check_button_get_active(GTK_CHECK_BUTTON(check))) {
        char *config_dir = g_build_filename(g_get_home_dir(),
                                            ".config", "prettymux", NULL);
        g_mkdir_with_parents(config_dir, 0755);
        char *flag_path = g_build_filename(config_dir, ".welcome-shown", NULL);
        g_file_set_contents(flag_path, "1", 1, NULL);
        g_free(flag_path);
        g_free(config_dir);
    }

    adw_dialog_close(dlg);
}

static void
show_welcome_dialog(GtkWindow *parent)
{
    char *flag_path = g_build_filename(g_get_home_dir(),
                                       ".config", "prettymux",
                                       ".welcome-shown", NULL);
    gboolean already_shown = g_file_test(flag_path, G_FILE_TEST_EXISTS);
    g_free(flag_path);

    if (already_shown)
        return;

    AdwDialog *dlg = adw_dialog_new();
    adw_dialog_set_title(dlg, "Welcome to PrettyMux");
    adw_dialog_set_content_width(dlg, 420);
    adw_dialog_set_content_height(dlg, 340);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_margin_start(box, 32);
    gtk_widget_set_margin_end(box, 32);
    gtk_widget_set_margin_top(box, 28);
    gtk_widget_set_margin_bottom(box, 24);

    /* Title */
    GtkWidget *title = gtk_label_new("Welcome to PrettyMux");
    gtk_widget_add_css_class(title, "title-1");
    gtk_label_set_xalign(GTK_LABEL(title), 0.5f);
    gtk_box_append(GTK_BOX(box), title);

    /* Description */
    GtkWidget *desc = gtk_label_new(
        "GPU-accelerated terminal multiplexer with\n"
        "ghostty + WebKit in one window.\n\n"
        "Press Ctrl+Shift+K to see all shortcuts.\n"
        "Visit prettymux-web.vercel.app for docs.");
    gtk_label_set_wrap(GTK_LABEL(desc), TRUE);
    gtk_label_set_xalign(GTK_LABEL(desc), 0.5f);
    gtk_label_set_justify(GTK_LABEL(desc), GTK_JUSTIFY_CENTER);
    gtk_box_append(GTK_BOX(box), desc);

    /* Spacer */
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(box), spacer);

    /* Checkbox */
    GtkWidget *check = gtk_check_button_new_with_label("Don't show this again");
    gtk_widget_set_halign(check, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), check);

    /* OK button */
    GtkWidget *ok_btn = gtk_button_new_with_label("Get Started");
    gtk_widget_add_css_class(ok_btn, "suggested-action");
    gtk_widget_set_halign(ok_btn, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(ok_btn, 140, -1);
    gtk_box_append(GTK_BOX(box), ok_btn);

    /* Store checkbox reference on the dialog for the callback */
    g_object_set_data(G_OBJECT(dlg), "check-button", check);

    g_signal_connect(ok_btn, "clicked",
                     G_CALLBACK(on_welcome_ok_clicked), dlg);

    adw_dialog_set_child(dlg, box);
    adw_dialog_present(dlg, GTK_WIDGET(parent));
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

    // Layout: overlay wraps everything so the command palette can float
    ui.overlay = gtk_overlay_new();
    gtk_window_set_child(GTK_WINDOW(window), ui.overlay);

    ui.outer_paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_overlay_set_child(GTK_OVERLAY(ui.overlay), ui.outer_paned);

    build_sidebar();
    gtk_paned_set_start_child(GTK_PANED(ui.outer_paned), ui.sidebar_box);
    gtk_paned_set_resize_start_child(GTK_PANED(ui.outer_paned), FALSE);
    gtk_paned_set_shrink_start_child(GTK_PANED(ui.outer_paned), FALSE);

    ui.main_paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_end_child(GTK_PANED(ui.outer_paned), ui.main_paned);

    // Terminal area: vertical box holding terminal_stack + notes panel
    ui.terminal_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(ui.terminal_box, TRUE);
    gtk_widget_set_vexpand(ui.terminal_box, TRUE);

    ui.terminal_stack = gtk_stack_new();
    gtk_widget_set_hexpand(ui.terminal_stack, TRUE);
    gtk_widget_set_vexpand(ui.terminal_stack, TRUE);
    gtk_box_append(GTK_BOX(ui.terminal_box), ui.terminal_stack);

    gtk_paned_set_start_child(GTK_PANED(ui.main_paned), ui.terminal_box);

    build_browser();
    gtk_paned_set_end_child(GTK_PANED(ui.main_paned), ui.browser_notebook);

    gtk_paned_set_position(GTK_PANED(ui.outer_paned), 200);
    gtk_paned_set_position(GTK_PANED(ui.main_paned), 700);

    // Command palette (overlay)
    ui.command_palette = command_palette_new(ui.browser_notebook,
                                             ui.terminal_stack,
                                             ui.workspace_list);
    gtk_widget_set_visible(ui.command_palette, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(ui.overlay), ui.command_palette);

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

    // ── Single instance socket server ──
    socket_server_set_callback(on_socket_command, NULL);
    const char *sock_path = socket_server_start();
    if (sock_path) {
        /* Set BASH_ENV to auto-source our shell integration script.
         * Try several locations: next to the executable, then next to
         * the source file (for development builds). */
        char exe_path[PATH_MAX];
        ssize_t exe_len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
        if (exe_len > 0) {
            exe_path[exe_len] = '\0';
            /* dirname modifies in-place, so work on a copy */
            char *exe_dir_buf = g_strdup(exe_path);
            const char *exe_dir = dirname(exe_dir_buf);

            char *shell_integ = g_build_filename(exe_dir, "prettymux-shell-integration.sh", NULL);
            if (!g_file_test(shell_integ, G_FILE_TEST_EXISTS)) {
                g_free(shell_integ);
                /* Fallback: next to source (development builds) */
                shell_integ = g_build_filename(PRETTYMUX_SOURCE_DIR,
                                               "prettymux-shell-integration.sh", NULL);
            }
            if (g_file_test(shell_integ, G_FILE_TEST_EXISTS)) {
                g_setenv("BASH_ENV", shell_integ, TRUE);
            }
            g_free(shell_integ);
            g_free(exe_dir_buf);
        }
    }

    // ── Port scanner ──
    port_scanner_set_callback(on_new_ports_detected, NULL);
    port_scanner_start();

    gtk_window_present(GTK_WINDOW(window));

    // ── Welcome dialog (first run) ──
    show_welcome_dialog(GTK_WINDOW(window));
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
