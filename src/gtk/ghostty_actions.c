#include "ghostty_actions.h"

#include <gtk/gtk.h>
#include <string.h>

#include "app_actions.h"
#include "app_state.h"
#include "app_support.h"
#include "ghostty_terminal.h"
#include "notifications.h"
#include "terminal_routing.h"
#include "workspace.h"

typedef struct {
    ghostty_surface_t surface;
    ghostty_action_s  action;
    char *str1;
    char *str2;
} ActionIdleData;

static void
action_idle_data_free(ActionIdleData *d)
{
    g_free(d->str1);
    g_free(d->str2);
    g_free(d);
}

static gboolean action_idle_handler(gpointer user_data);

bool
ghostty_actions_action_cb(ghostty_app_t app, ghostty_target_s target,
                          ghostty_action_s action)
{
    ghostty_surface_t surface = NULL;
    ActionIdleData *d;

    (void)app;

    if (target.tag == GHOSTTY_TARGET_SURFACE)
        surface = target.target.surface;

    d = g_new0(ActionIdleData, 1);
    d->surface = surface;
    d->action = action;

    switch (action.tag) {
    case GHOSTTY_ACTION_OPEN_URL:
        d->str1 = g_strdup(action.action.open_url.url);
        d->action.action.open_url.url = d->str1;
        break;
    case GHOSTTY_ACTION_MOUSE_OVER_LINK:
        d->str1 = g_strdup(action.action.mouse_over_link.url);
        d->action.action.mouse_over_link.url = d->str1;
        break;
    case GHOSTTY_ACTION_DESKTOP_NOTIFICATION:
        d->str1 = g_strdup(action.action.desktop_notification.body);
        d->str2 = g_strdup(action.action.desktop_notification.title);
        d->action.action.desktop_notification.body = d->str1;
        d->action.action.desktop_notification.title = d->str2;
        break;
    case GHOSTTY_ACTION_SET_TITLE:
        d->str1 = g_strdup(action.action.set_title.title);
        d->action.action.set_title.title = d->str1;
        break;
    case GHOSTTY_ACTION_PWD:
        d->str1 = g_strdup(action.action.pwd.pwd);
        d->action.action.pwd.pwd = d->str1;
        break;
    case GHOSTTY_ACTION_START_SEARCH:
        d->str1 = g_strdup(action.action.start_search.needle);
        d->action.action.start_search.needle = d->str1;
        break;
    default:
        break;
    }

    g_idle_add(action_idle_handler, d);
    return true;
}

static gboolean
action_idle_handler(gpointer user_data)
{
    ActionIdleData *d = user_data;
    ghostty_surface_t surface = d->surface;
    ghostty_action_s action = d->action;

    switch (action.tag) {
    case GHOSTTY_ACTION_OPEN_URL:
        if (action.action.open_url.url)
            app_actions_open_url_in_preferred_target(action.action.open_url.url);
        break;

    case GHOSTTY_ACTION_MOUSE_OVER_LINK: {
        SurfaceLookup loc = terminal_routing_find_for_surface(surface);
        if (loc.terminal)
            ghostty_terminal_set_hover_url(loc.terminal,
                                           action.action.mouse_over_link.url);
        break;
    }

    case GHOSTTY_ACTION_DESKTOP_NOTIFICATION: {
        SurfaceLookup loc = terminal_routing_find_for_surface(surface);
        send_desktop_notification("PrettyMux",
                                  action.action.desktop_notification.body,
                                  loc.workspace_idx,
                                  loc.pane_idx,
                                  loc.tab_idx);
        break;
    }

    case GHOSTTY_ACTION_SET_TITLE: {
        SurfaceLookup loc;

        if (!action.action.set_title.title || !action.action.set_title.title[0])
            break;
        loc = terminal_routing_find_for_surface(surface);
        if (loc.terminal) {
            ghostty_terminal_set_title(loc.terminal, action.action.set_title.title);
            if (loc.workspace) {
                snprintf(loc.workspace->name, sizeof(loc.workspace->name),
                         "%.60s", action.action.set_title.title);
                workspace_refresh_sidebar_label(loc.workspace);
            }
        }
        break;
    }

    case GHOSTTY_ACTION_PWD: {
        SurfaceLookup loc;

        if (!action.action.pwd.pwd || !action.action.pwd.pwd[0])
            break;
        loc = terminal_routing_find_for_surface(surface);
        if (loc.terminal) {
            ghostty_terminal_set_cwd(loc.terminal, action.action.pwd.pwd);
            if (loc.workspace) {
                snprintf(loc.workspace->cwd, sizeof(loc.workspace->cwd), "%s",
                         action.action.pwd.pwd);
                workspace_detect_git(loc.workspace);
                ghostty_terminal_set_status(loc.terminal,
                                            action.action.pwd.pwd,
                                            loc.workspace->git_branch);
            }
        }
        break;
    }

    case GHOSTTY_ACTION_COMMAND_FINISHED: {
        SurfaceLookup loc = terminal_routing_find_for_surface(surface);
        if (loc.terminal) {
            double secs;

            ghostty_terminal_notify_command_finished(
                loc.terminal,
                action.action.command_finished.exit_code,
                action.action.command_finished.duration);

            secs = action.action.command_finished.duration / 1000000000.0;
            if (secs > 3.0 && loc.workspace) {
                if (action.action.command_finished.exit_code == 0)
                    snprintf(loc.workspace->notification,
                             sizeof(loc.workspace->notification),
                             "Command done (%.1fs)", secs);
                else
                    snprintf(loc.workspace->notification,
                             sizeof(loc.workspace->notification),
                             "Exit %d (%.1fs)",
                             action.action.command_finished.exit_code, secs);
                workspace_refresh_sidebar_label(loc.workspace);
            }

            if (secs > 3.0) {
                char body[256];
                char notif_msg[256];
                const char *ws_name = loc.workspace ? loc.workspace->name : "?";
                const char *term_title = ghostty_terminal_get_title(loc.terminal);

                if (!term_title || !term_title[0])
                    term_title = "Terminal";

                if (action.action.command_finished.exit_code == 0)
                    snprintf(body, sizeof(body),
                             "Command finished in %s/%s (%.1fs)",
                             ws_name, term_title, secs);
                else
                    snprintf(body, sizeof(body),
                             "Command failed (exit %d) in %s/%s (%.1fs)",
                             action.action.command_finished.exit_code,
                             ws_name, term_title, secs);

                snprintf(notif_msg, sizeof(notif_msg),
                         "Command finished in %s/%s", ws_name, term_title);
                notifications_add_full(notif_msg,
                                       loc.workspace_idx,
                                       loc.pane_notebook,
                                       loc.tab_idx);
                bell_button_update();
                workspace_mark_tab_notification(loc.pane_notebook, loc.tab_idx);
                if (notification_target_is_active(loc.workspace_idx,
                                                  loc.pane_notebook,
                                                  loc.tab_idx)) {
                    debug_notification_log(
                        "notify route command suppressed active-target target=(%d,%d,%d) msg=%s",
                        loc.workspace_idx, loc.pane_idx, loc.tab_idx, notif_msg);
                } else if (g_main_window_active) {
                    debug_notification_log(
                        "notify route command toast active=%d target=(%d,%d,%d) msg=%s",
                        g_main_window_active,
                        loc.workspace_idx, loc.pane_idx, loc.tab_idx,
                        notif_msg);
                    sidebar_toast_show(notif_msg,
                                       loc.workspace_idx,
                                       loc.pane_notebook,
                                       loc.tab_idx);
                } else {
                    debug_notification_log(
                        "notify route command desktop active=%d target=(%d,%d,%d) msg=%s",
                        g_main_window_active,
                        loc.workspace_idx, loc.pane_idx, loc.tab_idx,
                        body);
                    send_desktop_notification("PrettyMux", body,
                                              loc.workspace_idx,
                                              loc.pane_idx,
                                              loc.tab_idx);
                }
            }
        }
        break;
    }

    case GHOSTTY_ACTION_RING_BELL: {
        SurfaceLookup loc = terminal_routing_find_for_surface(surface);
        char body[256];

        if (loc.terminal)
            ghostty_terminal_notify_bell(loc.terminal);

        {
            const char *ws_name = loc.workspace ? loc.workspace->name : "?";
            const char *term_title = loc.terminal
                ? ghostty_terminal_get_title(loc.terminal) : NULL;
            char notif_msg[256];

            if (!term_title || !term_title[0])
                term_title = "Terminal";
            snprintf(notif_msg, sizeof(notif_msg), "Bell in %s/%s",
                     ws_name, term_title);
            notifications_add_full(notif_msg,
                                   loc.workspace_idx,
                                   loc.pane_notebook,
                                   loc.tab_idx);
            bell_button_update();
            workspace_mark_tab_notification(loc.pane_notebook, loc.tab_idx);
        }

        if (!notification_target_is_active(loc.workspace_idx,
                                           loc.pane_notebook,
                                           loc.tab_idx)) {
            const char *ws_name = loc.workspace ? loc.workspace->name : "?";
            const char *term_title = loc.terminal
                ? ghostty_terminal_get_title(loc.terminal) : NULL;
            if (!term_title || !term_title[0])
                term_title = "Terminal";

            snprintf(body, sizeof(body), "Bell in %s/%s", ws_name, term_title);

            if (g_main_window_active) {
                debug_notification_log(
                    "notify route bell toast active=%d target=(%d,%d,%d) msg=%s",
                    g_main_window_active,
                    loc.workspace_idx, loc.pane_idx, loc.tab_idx,
                    body);
                sidebar_toast_show(body,
                                   loc.workspace_idx,
                                   loc.pane_notebook,
                                   loc.tab_idx);
            } else {
                debug_notification_log(
                    "notify route bell desktop active=%d target=(%d,%d,%d) msg=%s",
                    g_main_window_active,
                    loc.workspace_idx, loc.pane_idx, loc.tab_idx,
                    body);
                send_desktop_notification("PrettyMux", body,
                                          loc.workspace_idx,
                                          loc.pane_idx,
                                          loc.tab_idx);
            }
        } else {
            debug_notification_log(
                "notify route bell suppressed active-target target=(%d,%d,%d) msg=%s",
                loc.workspace_idx, loc.pane_idx, loc.tab_idx,
                body);
        }
        break;
    }

    case GHOSTTY_ACTION_RENDER: {
        SurfaceLookup loc = terminal_routing_find_for_surface(surface);
        if (loc.terminal) {
            ghostty_terminal_queue_render(loc.terminal);
            if (loc.workspace_idx >= 0 && loc.workspace_idx != current_workspace) {
                ghostty_terminal_mark_activity(loc.terminal);
                workspace_refresh_sidebar_label(loc.workspace);
            } else if (loc.workspace) {
                GtkNotebook *focused = workspace_get_focused_pane(loc.workspace);
                if (focused) {
                    int pg = gtk_notebook_get_current_page(focused);
                    GtkWidget *visible_terminal = (pg >= 0)
                        ? GTK_WIDGET(notebook_terminal_at(focused, pg))
                        : NULL;
                    if (visible_terminal != GTK_WIDGET(loc.terminal)) {
                        ghostty_terminal_mark_activity(loc.terminal);
                        workspace_refresh_tab_labels(loc.workspace);
                    }
                }
            }
        }
        break;
    }

    case GHOSTTY_ACTION_SHOW_CHILD_EXITED: {
        SurfaceLookup loc = terminal_routing_find_for_surface(surface);
        if (loc.terminal)
            ghostty_terminal_notify_child_exited(
                loc.terminal, action.action.child_exited.exit_code);
        break;
    }

    case GHOSTTY_ACTION_PROGRESS_REPORT: {
        SurfaceLookup loc = terminal_routing_find_for_surface(surface);
        if (loc.terminal && loc.workspace) {
            int pct = (int)action.action.progress_report.progress;
            int state = (int)action.action.progress_report.state;
            if (pct >= 0)
                snprintf(loc.workspace->notification,
                         sizeof(loc.workspace->notification),
                         "Progress: %d%%", pct);
            else
                loc.workspace->notification[0] = '\0';

            ghostty_terminal_set_progress(loc.terminal, state, pct);
            workspace_refresh_tab_labels(loc.workspace);
        }
        break;
    }

    case GHOSTTY_ACTION_START_SEARCH: {
        SurfaceLookup loc = terminal_routing_find_for_surface(surface);
        if (loc.terminal)
            terminal_search_show(loc.terminal, action.action.start_search.needle);
        break;
    }

    case GHOSTTY_ACTION_SEARCH_TOTAL: {
        SurfaceLookup loc = terminal_routing_find_for_surface(surface);
        if (loc.terminal == g_terminal_search_target) {
            g_terminal_search_total = action.action.search_total.total;
            ghostty_terminal_set_search_results(g_terminal_search_target,
                                                g_terminal_search_total,
                                                g_terminal_search_selected);
        }
        break;
    }

    case GHOSTTY_ACTION_SEARCH_SELECTED: {
        SurfaceLookup loc = terminal_routing_find_for_surface(surface);
        if (loc.terminal == g_terminal_search_target) {
            g_terminal_search_selected = action.action.search_selected.selected;
            ghostty_terminal_set_search_results(g_terminal_search_target,
                                                g_terminal_search_total,
                                                g_terminal_search_selected);
        }
        break;
    }

    default:
        break;
    }

    action_idle_data_free(d);
    return G_SOURCE_REMOVE;
}
