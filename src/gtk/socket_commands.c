#include "socket_commands.h"

#include <gtk/gtk.h>
#include <string.h>

#include "app_actions.h"
#include "app_state.h"
#include "app_support.h"
#include "notifications.h"
#include "session.h"
#include "shortcuts.h"
#include "socket_server.h"
#include "terminal_routing.h"
#include "workspace.h"
#include "workspace_layout.h"
#include "workspace_strip.h"

static char *
prettymux_trim_last_lines(const char *text, int lines)
{
    const char *start;
    const char *cursor;

    if (!text)
        return g_strdup("");
    if (lines <= 0)
        return g_strdup(text);

    start = text;
    cursor = text + strlen(text);
    while (cursor > start && lines > 0) {
        cursor--;
        if (*cursor == '\n' && cursor + 1 < text + strlen(text))
            lines--;
    }

    if (lines <= 0 && cursor > start)
        cursor++;
    else
        cursor = start;

    return g_strdup(cursor);
}

static GhosttyTerminal *
resolve_target_terminal(Workspace *ws,
                        int        pane_idx,
                        int        tab_idx,
                        const char *pane_id)
{
    GtkNotebook *nb = NULL;

    if (!ws)
        return NULL;

    if (pane_id && pane_id[0])
        nb = workspace_get_pane_by_id(ws, pane_id);
    if (!nb && pane_idx >= 0 && ws->pane_notebooks &&
        pane_idx < (int)ws->pane_notebooks->len) {
        nb = g_ptr_array_index(ws->pane_notebooks, pane_idx);
    }
    if (!nb)
        nb = workspace_get_focused_pane(ws);
    if (!nb)
        return NULL;

    {
        int pg = (tab_idx >= 0)
            ? tab_idx
            : gtk_notebook_get_current_page(GTK_NOTEBOOK(nb));
        if (pg >= 0 && pg < gtk_notebook_get_n_pages(GTK_NOTEBOOK(nb)))
            return notebook_terminal_at(GTK_NOTEBOOK(nb), pg);
    }

    return NULL;
}

static Workspace *
resolve_workspace_target(JsonObject *msg, int *out_idx)
{
    int idx = (int)json_object_get_int_member_with_default(
        msg, "workspace", current_workspace);

    if (!workspaces || idx < 0 || idx >= (int)workspaces->len)
        return NULL;

    if (out_idx)
        *out_idx = idx;
    return g_ptr_array_index(workspaces, idx);
}

static void
json_builder_add_workspace_status_entry(JsonBuilder *response,
                                        const workspace_status_entry *entry)
{
    json_builder_begin_object(response);
    json_builder_set_member_name(response, "entryId");
    json_builder_add_string_value(response, entry ? entry->entry_id : "");
    json_builder_set_member_name(response, "provider");
    json_builder_add_string_value(response, entry ? entry->provider : "");
    json_builder_set_member_name(response, "kind");
    json_builder_add_string_value(response, entry ? entry->kind : "");
    json_builder_set_member_name(response, "status");
    json_builder_add_string_value(response, entry ? entry->status : "");
    json_builder_set_member_name(response, "summary");
    json_builder_add_string_value(response, entry ? entry->summary : "");
    json_builder_set_member_name(response, "detail");
    json_builder_add_string_value(response, entry ? entry->detail : "");
    json_builder_set_member_name(response, "attention");
    json_builder_add_boolean_value(response, entry ? entry->attention : FALSE);
    json_builder_set_member_name(response, "updatedAtUsec");
    json_builder_add_int_value(response, entry ? entry->updated_at_usec : 0);
    json_builder_end_object(response);
}

void
socket_commands_on_socket_command(const char  *command,
                                  JsonObject  *msg,
                                  JsonBuilder *response,
                                  gpointer     user_data)
{
    const char *target_instance;
    const char *local_instance;

    (void)user_data;
    target_instance =
        json_object_get_string_member_with_default(msg, "instanceId", "");
    local_instance = app_state_get_instance_id();

    if (target_instance[0] && g_strcmp0(target_instance, local_instance) != 0) {
        g_autoptr(GError) route_error = NULL;

        if (socket_server_route_command_to_instance(target_instance, msg,
                                                    response, &route_error))
            return;

        json_builder_set_member_name(response, "status");
        json_builder_add_string_value(response, "error");
        json_builder_set_member_name(response, "message");
        json_builder_add_string_value(
            response,
            (route_error && route_error->message)
                ? route_error->message
                : "failed to route command to requested instance");
        return;
    }

    if (strcmp(command, "tabs.list") == 0) {
        json_builder_set_member_name(response, "status");
        json_builder_add_string_value(response, "ok");
        json_builder_set_member_name(response, "activeWorkspace");
        json_builder_add_int_value(response, current_workspace);
        json_builder_set_member_name(response, "workspaces");
        json_builder_begin_array(response);
        if (workspaces) {
            for (guint wi = 0; wi < workspaces->len; wi++) {
                Workspace *ws = g_ptr_array_index(workspaces, wi);
                json_builder_begin_object(response);
                json_builder_set_member_name(response, "index");
                json_builder_add_int_value(response, (int)wi);
                json_builder_set_member_name(response, "name");
                json_builder_add_string_value(response, ws->name);
                json_builder_set_member_name(response, "active");
                json_builder_add_boolean_value(response,
                                               (int)wi == current_workspace);
                json_builder_set_member_name(response, "panes");
                json_builder_begin_array(response);
                if (ws->pane_notebooks) {
                    GtkNotebook *focused_pane = workspace_get_focused_pane(ws);
                    for (guint pi = 0; pi < ws->pane_notebooks->len; pi++) {
                        GtkNotebook *nb = g_ptr_array_index(ws->pane_notebooks, pi);
                        json_builder_begin_object(response);
                        json_builder_set_member_name(response, "id");
                        json_builder_add_string_value(response,
                                                      workspace_get_pane_id(nb));
                        json_builder_set_member_name(response, "index");
                        json_builder_add_int_value(response, (int)pi);
                        json_builder_set_member_name(response, "focused");
                        json_builder_add_boolean_value(response, nb == focused_pane);
                        json_builder_set_member_name(response, "activeTab");
                        json_builder_add_int_value(response,
                                                   GTK_IS_NOTEBOOK(nb)
                                                       ? gtk_notebook_get_current_page(nb)
                                                       : -1);
                        json_builder_set_member_name(response, "tabs");
                        json_builder_begin_array(response);
                        if (GTK_IS_NOTEBOOK(nb)) {
                            int n_pages = gtk_notebook_get_n_pages(nb);
                            for (int ti = 0; ti < n_pages; ti++) {
                                GtkWidget *child =
                                    gtk_notebook_get_nth_page(nb, ti);
                                const char *tab_name = "Terminal";
                                gboolean is_custom = FALSE;
                                GtkWidget *tab_widget =
                                    gtk_notebook_get_tab_label(nb, child);
                                if (tab_widget) {
                                    for (GtkWidget *w = gtk_widget_get_first_child(tab_widget);
                                         w; w = gtk_widget_get_next_sibling(w)) {
                                        if (GTK_IS_LABEL(w)) {
                                            tab_name = gtk_label_get_text(GTK_LABEL(w));
                                            if (g_object_get_data(G_OBJECT(w), "user-renamed"))
                                                is_custom = TRUE;
                                            break;
                                        }
                                    }
                                }

                                {
                                    GhosttyTerminal *terminal = notebook_terminal_at(nb, ti);
                                    const char *pwd = terminal
                                        ? ghostty_terminal_get_cwd(terminal)
                                        : NULL;

                                    json_builder_begin_object(response);
                                    json_builder_set_member_name(response, "index");
                                    json_builder_add_int_value(response, ti);
                                    json_builder_set_member_name(response, "name");
                                    json_builder_add_string_value(response,
                                                                  tab_name ? tab_name : "Terminal");
                                    json_builder_set_member_name(response, "customName");
                                    json_builder_add_boolean_value(response, is_custom);
                                    json_builder_set_member_name(response, "pwd");
                                    json_builder_add_string_value(response, pwd ? pwd : "");
                                    json_builder_set_member_name(response, "active");
                                    json_builder_add_boolean_value(response,
                                                                   ti == gtk_notebook_get_current_page(nb));
                                    json_builder_end_object(response);
                                }
                            }
                        }
                        json_builder_end_array(response);
                        json_builder_end_object(response);
                    }
                }
                json_builder_end_array(response);
                json_builder_end_object(response);
            }
        }
        json_builder_end_array(response);
    } else if (strcmp(command, "browser.open") == 0) {
        const char *url = json_object_get_string_member_with_default(msg, "url", "");
        if (url && url[0]) {
            app_actions_open_url_in_preferred_target(url);
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "ok");
        } else {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "error");
            json_builder_set_member_name(response, "message");
            json_builder_add_string_value(response, "missing url");
        }
    } else if (strcmp(command, "workspace.new") == 0) {
        const char *name = json_object_get_string_member_with_default(msg, "name", "");
        workspace_add(ui.terminal_stack, ui.workspace_list, g_ghostty_app);
        if (name && name[0] && workspaces && workspaces->len > 0) {
            Workspace *ws = g_ptr_array_index(workspaces, workspaces->len - 1);
            snprintf(ws->name, sizeof(ws->name), "%.60s", name);
            ws->user_renamed = TRUE;
            workspace_refresh_sidebar_label(ws);
        }
        json_builder_set_member_name(response, "status");
        json_builder_add_string_value(response, "ok");
        json_builder_set_member_name(response, "index");
        json_builder_add_int_value(response,
                                   workspaces ? (int)workspaces->len - 1 : 0);
    } else if (strcmp(command, "workspace.list") == 0) {
        json_builder_set_member_name(response, "status");
        json_builder_add_string_value(response, "ok");
        json_builder_set_member_name(response, "workspaces");
        json_builder_begin_array(response);
        if (workspaces) {
            for (guint i = 0; i < workspaces->len; i++) {
                Workspace *ws = g_ptr_array_index(workspaces, i);
                json_builder_begin_object(response);
                json_builder_set_member_name(response, "index");
                json_builder_add_int_value(response, (int)i);
                json_builder_set_member_name(response, "name");
                json_builder_add_string_value(response, ws->name);
                json_builder_set_member_name(response, "active");
                json_builder_add_boolean_value(response,
                                               (int)i == current_workspace);
                json_builder_end_object(response);
            }
        }
        json_builder_end_array(response);
    } else if (strcmp(command, "workspace.current") == 0) {
        Workspace *ws = workspace_get_current();
        GtkNotebook *pane = ws ? workspace_get_focused_pane(ws) : NULL;
        json_builder_set_member_name(response, "status");
        json_builder_add_string_value(response, "ok");
        json_builder_set_member_name(response, "index");
        json_builder_add_int_value(response, current_workspace);
        json_builder_set_member_name(response, "name");
        json_builder_add_string_value(response, ws ? ws->name : "");
        json_builder_set_member_name(response, "paneId");
        json_builder_add_string_value(response,
                                      pane ? workspace_get_pane_id(pane) : "");
        json_builder_set_member_name(response, "paneIndex");
        json_builder_add_int_value(response,
                                   (ws && pane)
                                       ? workspace_get_pane_index(ws, pane)
                                       : -1);
    } else if (strcmp(command, "workspace.switch") == 0) {
        int idx = (int)json_object_get_int_member_with_default(msg, "index", -1);
        if (idx >= 0 && workspaces && idx < (int)workspaces->len) {
            workspace_switch(idx, ui.terminal_stack, ui.workspace_list);
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "ok");
        } else {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "error");
            json_builder_set_member_name(response, "message");
            json_builder_add_string_value(response, "invalid workspace index");
        }
    } else if (strcmp(command, "workspace.import") == 0) {
        const char *payload = json_object_get_string_member_with_default(
            msg, "workspacePayload", "");
        g_autofree char *import_error = NULL;
        int imported_idx = -1;

        if (workspace_import_from_payload(payload, g_ghostty_app, &imported_idx,
                                          &import_error)) {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "ok");
            json_builder_set_member_name(response, "index");
            json_builder_add_int_value(response, imported_idx);
        } else {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "error");
            json_builder_set_member_name(response, "message");
            json_builder_add_string_value(
                response,
                import_error ? import_error : "workspace import failed");
        }
    } else if (strcmp(command, "workspace.move_to_instance") == 0) {
        int idx = (int)json_object_get_int_member_with_default(
            msg, "workspace", current_workspace);
        const char *target_instance_id =
            json_object_get_string_member_with_default(msg, "targetInstanceId",
                                                       "");
        g_autofree char *move_error = NULL;
        int target_ws_idx = -1;

        if (workspace_move_to_instance(idx, target_instance_id, &target_ws_idx,
                                       &move_error)) {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "ok");
            json_builder_set_member_name(response, "workspace");
            json_builder_add_int_value(response, idx);
            json_builder_set_member_name(response, "targetInstanceId");
            json_builder_add_string_value(response, target_instance_id);
            json_builder_set_member_name(response, "targetWorkspace");
            json_builder_add_int_value(response, target_ws_idx);
        } else {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "error");
            json_builder_set_member_name(response, "message");
            json_builder_add_string_value(
                response,
                move_error ? move_error : "workspace move failed");
        }
    } else if (strcmp(command, "workspace.set_layout") == 0) {
        gboolean has_workspace_target = json_object_has_member(msg, "workspace");
        int idx = (int)json_object_get_int_member_with_default(
            msg, "workspace", current_workspace);
        const char *layout =
            json_object_get_string_member_with_default(msg, "layout", "");
        Workspace *ws = NULL;
        WorkspaceLayoutMode mode = WORKSPACE_LAYOUT_CLASSIC;
        int updated_count = 0;

        if (has_workspace_target &&
            idx >= 0 && workspaces &&
            idx < (int)workspaces->len) {
            ws = g_ptr_array_index(workspaces, idx);
        }

        if (g_strcmp0(layout, "strip") == 0)
            mode = WORKSPACE_LAYOUT_STRIP;
        else if (g_strcmp0(layout, "classic") == 0)
            mode = WORKSPACE_LAYOUT_CLASSIC;
        else {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "error");
            json_builder_set_member_name(response, "message");
            json_builder_add_string_value(response, "invalid layout (use 'classic' or 'strip')");
            goto set_layout_done;
        }

        if (!has_workspace_target) {
            if (!workspaces || workspaces->len == 0) {
                json_builder_set_member_name(response, "status");
                json_builder_add_string_value(response, "error");
                json_builder_set_member_name(response, "message");
                json_builder_add_string_value(response,
                                              "no workspaces available");
                goto set_layout_done;
            }

            updated_count = workspace_apply_layout_mode_to_all(mode);
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "ok");
            json_builder_set_member_name(response, "layout");
            json_builder_add_string_value(response, layout);
            json_builder_set_member_name(response, "scope");
            json_builder_add_string_value(response, "all");
            json_builder_set_member_name(response, "workspaceCount");
            json_builder_add_int_value(response, (int)workspaces->len);
            json_builder_set_member_name(response, "updatedCount");
            json_builder_add_int_value(response, updated_count);
            goto set_layout_done;
        }

        if (!ws) {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "error");
            json_builder_set_member_name(response, "message");
            json_builder_add_string_value(response, "invalid workspace index");
            goto set_layout_done;
        }

        if (workspace_rebuild_for_layout_mode(ws, mode)) {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "ok");
            json_builder_set_member_name(response, "layout");
            json_builder_add_string_value(response, layout);
            json_builder_set_member_name(response, "scope");
            json_builder_add_string_value(response, "workspace");
            json_builder_set_member_name(response, "workspace");
            json_builder_add_int_value(response, idx);
        } else {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "error");
            json_builder_set_member_name(response, "message");
            json_builder_add_string_value(response, "set_layout failed");
        }
    set_layout_done:
        (void)0;
    } else if (strcmp(command, "workspace.get_layout") == 0) {
        int idx = (int)json_object_get_int_member_with_default(msg, "workspace", current_workspace);
        Workspace *ws = NULL;

        if (idx >= 0 && workspaces && idx < (int)workspaces->len)
            ws = g_ptr_array_index(workspaces, idx);

        if (ws) {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "ok");
            json_builder_set_member_name(response, "layout");
            if (workspace_get_layout_mode(ws) == WORKSPACE_LAYOUT_STRIP)
                json_builder_add_string_value(response, "strip");
            else
                json_builder_add_string_value(response, "classic");
        } else {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "error");
            json_builder_set_member_name(response, "message");
            json_builder_add_string_value(response, "invalid workspace index");
        }
    } else if (strcmp(command, "workspace.get_strip_state") == 0) {
        int idx = (int)json_object_get_int_member_with_default(
            msg, "workspace", current_workspace);
        Workspace *ws = NULL;
        WorkspaceStripState *state = NULL;

        if (idx >= 0 && workspaces && idx < (int)workspaces->len)
            ws = g_ptr_array_index(workspaces, idx);

        if (!ws) {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "error");
            json_builder_set_member_name(response, "message");
            json_builder_add_string_value(response, "invalid workspace index");
        } else if (workspace_get_layout_mode(ws) != WORKSPACE_LAYOUT_STRIP) {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "error");
            json_builder_set_member_name(response, "message");
            json_builder_add_string_value(response, "workspace layout is not strip");
        } else {
            int focused_col = 0;

            state = ws->strip_state;
            if (state && state->focused_col >= 0)
                focused_col = state->focused_col;

            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "ok");
            json_builder_set_member_name(response, "layout");
            json_builder_add_string_value(response, "strip");
            json_builder_set_member_name(response, "focusedColumn");
            json_builder_add_int_value(response, focused_col);
            json_builder_set_member_name(response, "columns");
            json_builder_begin_array(response);
            if (state && state->columns) {
                for (guint i = 0; i < state->columns->len; i++) {
                    WorkspaceColumn *col = g_ptr_array_index(state->columns, i);
                    const char *pane_id = NULL;
                    int width = 600;

                    if (!col)
                        continue;
                    if (col->target_width > 0)
                        width = col->target_width;
                    {
                        GtkNotebook *fnb = workspace_strip_column_focused_notebook(col);
                        if (fnb)
                            pane_id = workspace_get_pane_id(fnb);
                        else if (GTK_IS_NOTEBOOK(col->notebook))
                            pane_id = workspace_get_pane_id(GTK_NOTEBOOK(col->notebook));
                    }

                    json_builder_begin_object(response);
                    json_builder_set_member_name(response, "index");
                    json_builder_add_int_value(response, (int)i);
                    json_builder_set_member_name(response, "paneId");
                    json_builder_add_string_value(response, pane_id ? pane_id : "");
                    json_builder_set_member_name(response, "width");
                    json_builder_add_int_value(response, width);
                    json_builder_set_member_name(response, "maximized");
                    json_builder_add_boolean_value(response, col->maximized);
                    json_builder_set_member_name(response, "focused");
                    json_builder_add_boolean_value(response, (int)i == focused_col);
                    json_builder_set_member_name(response, "paneCount");
                    json_builder_add_int_value(response,
                        col->panes ? (int)col->panes->len : 1);
                    json_builder_set_member_name(response, "focusedPane");
                    json_builder_add_int_value(response, col->focused_pane);
                    json_builder_end_object(response);
                }
            }
            json_builder_end_array(response);
        }
    } else if (strcmp(command, "workspace.status.set") == 0) {
        Workspace *ws = NULL;
        int ws_idx = -1;
        const char *entry_id =
            json_object_get_string_member_with_default(msg, "entryId", "");
        gboolean should_notify =
            json_object_get_boolean_member_with_default(msg, "notify", FALSE);
        gint64 updated_usec =
            json_object_get_int_member_with_default(msg, "updatedAtUsec", 0);
        gint64 updated_ms =
            json_object_get_int_member_with_default(msg, "updatedAtMs", 0);

        ws = resolve_workspace_target(msg, &ws_idx);
        if (!ws) {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "error");
            json_builder_set_member_name(response, "message");
            json_builder_add_string_value(response, "invalid workspace index");
        } else if (!entry_id[0]) {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "error");
            json_builder_set_member_name(response, "message");
            json_builder_add_string_value(response, "missing entryId");
        } else {
            workspace_status_entry entry = {0};

            g_strlcpy(entry.entry_id, entry_id, sizeof(entry.entry_id));
            g_strlcpy(entry.provider,
                      json_object_get_string_member_with_default(
                          msg, "provider", ""),
                      sizeof(entry.provider));
            g_strlcpy(entry.kind,
                      json_object_get_string_member_with_default(
                          msg, "kind", ""),
                      sizeof(entry.kind));
            g_strlcpy(entry.status,
                      json_object_get_string_member_with_default(
                          msg, "state", ""),
                      sizeof(entry.status));
            g_strlcpy(entry.summary,
                      json_object_get_string_member_with_default(
                          msg, "summary", ""),
                      sizeof(entry.summary));
            g_strlcpy(entry.detail,
                      json_object_get_string_member_with_default(
                          msg, "detail", ""),
                      sizeof(entry.detail));
            entry.attention =
                json_object_get_boolean_member_with_default(msg, "attention",
                                                            FALSE);
            if (updated_usec > 0)
                entry.updated_at_usec = updated_usec;
            else if (updated_ms > 0)
                entry.updated_at_usec = updated_ms * 1000;
            else
                entry.updated_at_usec = g_get_real_time();

            workspace_set_status_entry(ws, &entry);
            if (should_notify)
                notifications_publish_workspace_status(ws, &entry, TRUE);

            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "ok");
            json_builder_set_member_name(response, "workspace");
            json_builder_add_int_value(response, ws_idx);
            json_builder_set_member_name(response, "entryId");
            json_builder_add_string_value(response, entry.entry_id);
        }
    } else if (strcmp(command, "workspace.status.clear") == 0) {
        Workspace *ws = NULL;
        int ws_idx = -1;
        const char *entry_id =
            json_object_get_string_member_with_default(msg, "entryId", "");

        ws = resolve_workspace_target(msg, &ws_idx);
        if (!ws) {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "error");
            json_builder_set_member_name(response, "message");
            json_builder_add_string_value(response, "invalid workspace index");
        } else if (!entry_id[0]) {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "error");
            json_builder_set_member_name(response, "message");
            json_builder_add_string_value(response, "missing entryId");
        } else {
            workspace_clear_status_entry(ws, entry_id);
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "ok");
            json_builder_set_member_name(response, "workspace");
            json_builder_add_int_value(response, ws_idx);
            json_builder_set_member_name(response, "entryId");
            json_builder_add_string_value(response, entry_id);
        }
    } else if (strcmp(command, "workspace.status.list") == 0) {
        Workspace *ws = NULL;
        int ws_idx = -1;
        g_autoptr(GPtrArray) entries = NULL;

        ws = resolve_workspace_target(msg, &ws_idx);
        if (!ws) {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "error");
            json_builder_set_member_name(response, "message");
            json_builder_add_string_value(response, "invalid workspace index");
        } else {
            entries = workspace_get_sorted_status_entries(ws);

            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "ok");
            json_builder_set_member_name(response, "workspace");
            json_builder_add_int_value(response, ws_idx);
            json_builder_set_member_name(response, "entries");
            json_builder_begin_array(response);
            if (entries) {
                for (guint i = 0; i < entries->len; i++) {
                    workspace_status_entry *entry =
                        g_ptr_array_index(entries, i);
                    json_builder_add_workspace_status_entry(response, entry);
                }
            }
            json_builder_end_array(response);
        }
    } else if (strcmp(command, "workspace.equalize_splits") == 0) {
        int idx = (int)json_object_get_int_member_with_default(msg, "workspace", current_workspace);
        const char *orientation =
            json_object_get_string_member_with_default(msg, "orientation", "");
        Workspace *ws = NULL;

        if (idx >= 0 && workspaces && idx < (int)workspaces->len)
            ws = g_ptr_array_index(workspaces, idx);

        if (ws && workspace_equalize_splits(ws,
                                            (orientation && orientation[0])
                                                ? orientation
                                                : NULL)) {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "ok");
        } else {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "error");
            json_builder_set_member_name(response, "message");
            json_builder_add_string_value(response, "equalize failed");
        }
    } else if (strcmp(command, "pane.list") == 0) {
        int idx = (int)json_object_get_int_member_with_default(msg, "workspace", current_workspace);
        Workspace *ws = NULL;

        if (idx >= 0 && workspaces && idx < (int)workspaces->len)
            ws = g_ptr_array_index(workspaces, idx);

        if (!ws) {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "error");
            json_builder_set_member_name(response, "message");
            json_builder_add_string_value(response, "invalid workspace");
        } else {
            GtkNotebook *focused_pane = workspace_get_focused_pane(ws);
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "ok");
            json_builder_set_member_name(response, "workspace");
            json_builder_add_int_value(response, idx);
            json_builder_set_member_name(response, "activePane");
            json_builder_add_int_value(response,
                                       focused_pane ? workspace_get_pane_index(ws, focused_pane) : -1);
            json_builder_set_member_name(response, "activePaneId");
            json_builder_add_string_value(response,
                                          focused_pane ? workspace_get_pane_id(focused_pane) : "");
            json_builder_set_member_name(response, "panes");
            json_builder_begin_array(response);
            if (ws->pane_notebooks) {
                for (guint pi = 0; pi < ws->pane_notebooks->len; pi++) {
                    GtkNotebook *nb = g_ptr_array_index(ws->pane_notebooks, pi);
                    json_builder_begin_object(response);
                    json_builder_set_member_name(response, "id");
                    json_builder_add_string_value(response, workspace_get_pane_id(nb));
                    json_builder_set_member_name(response, "index");
                    json_builder_add_int_value(response, (int)pi);
                    json_builder_set_member_name(response, "focused");
                    json_builder_add_boolean_value(response, nb == focused_pane);
                    json_builder_set_member_name(response, "activeTab");
                    json_builder_add_int_value(response, gtk_notebook_get_current_page(nb));
                    json_builder_set_member_name(response, "tabCount");
                    json_builder_add_int_value(response, gtk_notebook_get_n_pages(nb));
                    json_builder_end_object(response);
                }
            }
            json_builder_end_array(response);
        }
    } else if (strcmp(command, "pane.focus") == 0) {
        int idx = (int)json_object_get_int_member_with_default(msg, "workspace", current_workspace);
        int pane_idx = (int)json_object_get_int_member_with_default(msg, "pane", -1);
        const char *pane_id =
            json_object_get_string_member_with_default(msg, "paneId", "");
        Workspace *ws = NULL;
        GtkNotebook *pane = NULL;

        if (idx >= 0 && workspaces && idx < (int)workspaces->len)
            ws = g_ptr_array_index(workspaces, idx);
        if (ws && pane_id && pane_id[0])
            pane = workspace_get_pane_by_id(ws, pane_id);
        if (ws && !pane && pane_idx >= 0)
            pane = workspace_get_pane_by_index(ws, pane_idx);

        if (ws && pane && workspace_focus_pane(ws, pane)) {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "ok");
        } else {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "error");
            json_builder_set_member_name(response, "message");
            json_builder_add_string_value(response, "focus failed");
        }
    } else if (strcmp(command, "pane.split") == 0) {
        int idx = (int)json_object_get_int_member_with_default(msg, "workspace", current_workspace);
        int pane_idx = (int)json_object_get_int_member_with_default(msg, "pane", -1);
        const char *pane_id =
            json_object_get_string_member_with_default(msg, "paneId", "");
        const char *direction =
            json_object_get_string_member_with_default(msg, "direction", "right");
        Workspace *ws = NULL;
        GtkNotebook *pane = NULL;
        GtkNotebook *new_pane = NULL;
        GtkOrientation orientation = GTK_ORIENTATION_HORIZONTAL;

        if (idx >= 0 && workspaces && idx < (int)workspaces->len)
            ws = g_ptr_array_index(workspaces, idx);
        if (ws && pane_id && pane_id[0])
            pane = workspace_get_pane_by_id(ws, pane_id);
        if (ws && !pane && pane_idx >= 0)
            pane = workspace_get_pane_by_index(ws, pane_idx);

        if (g_strcmp0(direction, "down") == 0 || g_strcmp0(direction, "up") == 0)
            orientation = GTK_ORIENTATION_VERTICAL;

        if (ws && pane)
            new_pane = workspace_split_pane_target(ws, pane, orientation, g_ghostty_app);

        if (ws && new_pane) {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "ok");
            json_builder_set_member_name(response, "paneId");
            json_builder_add_string_value(response, workspace_get_pane_id(new_pane));
            json_builder_set_member_name(response, "pane");
            json_builder_add_int_value(response, workspace_get_pane_index(ws, new_pane));
        } else {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "error");
            json_builder_set_member_name(response, "message");
            json_builder_add_string_value(response, "split failed");
        }
    } else if (strcmp(command, "pane.close") == 0) {
        int idx = (int)json_object_get_int_member_with_default(msg, "workspace", current_workspace);
        int pane_idx = (int)json_object_get_int_member_with_default(msg, "pane", -1);
        const char *pane_id =
            json_object_get_string_member_with_default(msg, "paneId", "");
        Workspace *ws = NULL;
        GtkNotebook *pane = NULL;

        if (idx >= 0 && workspaces && idx < (int)workspaces->len)
            ws = g_ptr_array_index(workspaces, idx);
        if (ws && pane_id && pane_id[0])
            pane = workspace_get_pane_by_id(ws, pane_id);
        if (ws && !pane && pane_idx >= 0)
            pane = workspace_get_pane_by_index(ws, pane_idx);

        if (ws && pane && ws->pane_notebooks && ws->pane_notebooks->len > 1) {
            workspace_close_pane(ws, pane);
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "ok");
        } else {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "error");
            json_builder_set_member_name(response, "message");
            json_builder_add_string_value(response, "close failed");
        }
    } else if (strcmp(command, "pane.resize_percent") == 0) {
        int idx = (int)json_object_get_int_member_with_default(msg, "workspace", current_workspace);
        int pane_idx = (int)json_object_get_int_member_with_default(msg, "pane", -1);
        const char *pane_id =
            json_object_get_string_member_with_default(msg, "paneId", "");
        const char *axis =
            json_object_get_string_member_with_default(msg, "axis", "x");
        double percent = json_object_get_double_member_with_default(msg, "percent", -1.0);
        Workspace *ws = NULL;
        GtkNotebook *pane = NULL;

        if (idx >= 0 && workspaces && idx < (int)workspaces->len)
            ws = g_ptr_array_index(workspaces, idx);
        if (ws && pane_id && pane_id[0])
            pane = workspace_get_pane_by_id(ws, pane_id);
        if (ws && !pane && pane_idx >= 0)
            pane = workspace_get_pane_by_index(ws, pane_idx);

        if (ws && pane && workspace_resize_pane_percent(ws, pane, axis[0], percent)) {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "ok");
        } else {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "error");
            json_builder_set_member_name(response, "message");
            json_builder_add_string_value(response, "resize failed");
        }
    } else if (strcmp(command, "tab.new") == 0) {
        Workspace *ws = workspace_get_current();
        if (ws) {
            workspace_add_terminal_to_focused(ws, g_ghostty_app);
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "ok");
        } else {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "error");
        }
    } else if (strcmp(command, "tab.move") == 0) {
        int from_ws = (int)json_object_get_int_member_with_default(msg, "fromWorkspace", -1);
        int from_pane = (int)json_object_get_int_member_with_default(msg, "fromPane", -1);
        int from_tab = (int)json_object_get_int_member_with_default(msg, "fromTab", -1);
        int to_ws = (int)json_object_get_int_member_with_default(msg, "toWorkspace", -1);
        int to_pane = (int)json_object_get_int_member_with_default(msg, "toPane", -1);
        if (workspace_move_tab(from_ws, from_pane, from_tab, to_ws, to_pane)) {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "ok");
        } else {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "error");
            json_builder_set_member_name(response, "message");
            json_builder_add_string_value(response, "move failed");
        }
    } else if (strcmp(command, "tab.select") == 0) {
        int ws_idx = (int)json_object_get_int_member_with_default(msg, "workspace", -1);
        int pane_idx = (int)json_object_get_int_member_with_default(msg, "pane", -1);
        int tab_idx = (int)json_object_get_int_member_with_default(msg, "tab", -1);
        if (workspace_select_tab(ws_idx, pane_idx, tab_idx)) {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "ok");
        } else {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "error");
            json_builder_set_member_name(response, "message");
            json_builder_add_string_value(response, "select failed");
        }
    } else if (strcmp(command, "tab.rename") == 0) {
        const char *name = json_object_get_string_member_with_default(msg, "name", "");
        if (name && name[0]) {
            Workspace *ws = workspace_get_current();
            if (ws) {
                GtkNotebook *nb = workspace_get_focused_pane(ws);
                if (nb && GTK_IS_NOTEBOOK(nb)) {
                    int pg = gtk_notebook_get_current_page(nb);
                    if (pg >= 0) {
                        GtkWidget *child = gtk_notebook_get_nth_page(nb, pg);
                        GtkWidget *tab_w = gtk_notebook_get_tab_label(nb, child);
                        if (tab_w) {
                            for (GtkWidget *w = gtk_widget_get_first_child(tab_w);
                                 w; w = gtk_widget_get_next_sibling(w)) {
                                if (GTK_IS_LABEL(w)) {
                                    gtk_label_set_text(GTK_LABEL(w), name);
                                    g_object_set_data(G_OBJECT(w), "user-renamed",
                                                      GINT_TO_POINTER(1));
                                    break;
                                }
                            }
                        }
                    }
                }
            }
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "ok");
        } else {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "error");
            json_builder_set_member_name(response, "message");
            json_builder_add_string_value(response, "missing name");
        }
    } else if (strcmp(command, "tab.edit") == 0) {
        Workspace *ws = workspace_get_current();
        if (ws) {
            workspace_start_tab_rename(ws);
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "ok");
        } else {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "error");
        }
    } else if (strcmp(command, "workspace.edit") == 0) {
        int idx = (int)json_object_get_int_member_with_default(msg,
                                                                "workspace",
                                                                -1);
        Workspace *ws = NULL;
        if (idx >= 0 && workspaces && idx < (int)workspaces->len)
            ws = g_ptr_array_index(workspaces, idx);
        else
            ws = workspace_get_current();
        if (ws) {
            workspace_start_workspace_rename(ws);
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "ok");
        } else {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "error");
            json_builder_set_member_name(response, "message");
            json_builder_add_string_value(response, "no workspace");
        }
    } else if (strcmp(command, "action") == 0) {
        const char *act = json_object_get_string_member_with_default(msg, "action", "");
        gboolean non_interactive =
            json_object_get_boolean_member_with_default(msg, "nonInteractive",
                                                        FALSE);
        if (act && act[0]) {
            const char *error_message = NULL;
            if (app_actions_handle_for_socket(act, non_interactive,
                                              &error_message)) {
                json_builder_set_member_name(response, "status");
                json_builder_add_string_value(response, "ok");
            } else {
                json_builder_set_member_name(response, "status");
                json_builder_add_string_value(response, "error");
                json_builder_set_member_name(response, "message");
                json_builder_add_string_value(
                    response, error_message ? error_message : "action failed");
            }
        } else {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "error");
            json_builder_set_member_name(response, "message");
            json_builder_add_string_value(response, "missing action name");
        }
    } else if (strcmp(command, "type") == 0) {
        const char *text = json_object_get_string_member_with_default(msg, "text", "");
        int ws_idx = (int)json_object_get_int_member_with_default(msg, "workspace", -1);
        int pane_idx = (int)json_object_get_int_member_with_default(msg, "pane", -1);
        int tab_idx = (int)json_object_get_int_member_with_default(msg, "tab", -1);
        const char *pane_id =
            json_object_get_string_member_with_default(msg, "paneId", "");
        Workspace *ws = NULL;

        if (ws_idx >= 0 && workspaces && ws_idx < (int)workspaces->len)
            ws = g_ptr_array_index(workspaces, ws_idx);
        else
            ws = workspace_get_current();

        if (ws && text && text[0]) {
            GhosttyTerminal *term = resolve_target_terminal(ws, pane_idx, tab_idx, pane_id);
            if (term) {
                ghostty_surface_t surface = ghostty_terminal_get_surface(term);
                if (surface) {
                    ghostty_surface_text(surface, text, strlen(text));
                    json_builder_set_member_name(response, "status");
                    json_builder_add_string_value(response, "ok");
                } else {
                    json_builder_set_member_name(response, "status");
                    json_builder_add_string_value(response, "error");
                    json_builder_set_member_name(response, "message");
                    json_builder_add_string_value(response, "terminal has no surface");
                }
            } else {
                json_builder_set_member_name(response, "status");
                json_builder_add_string_value(response, "error");
                json_builder_set_member_name(response, "message");
                json_builder_add_string_value(response, "no terminal found");
            }
        } else {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "error");
            json_builder_set_member_name(response, "message");
            json_builder_add_string_value(response, "missing text");
        }
    } else if (strcmp(command, "exec") == 0) {
        const char *cmd = json_object_get_string_member_with_default(msg, "cmd", "");
        int ws_idx = (int)json_object_get_int_member_with_default(msg, "workspace", -1);
        int pane_idx = (int)json_object_get_int_member_with_default(msg, "pane", -1);
        int tab_idx = (int)json_object_get_int_member_with_default(msg, "tab", -1);
        const char *pane_id =
            json_object_get_string_member_with_default(msg, "paneId", "");
        Workspace *ws = NULL;

        if (ws_idx >= 0 && workspaces && ws_idx < (int)workspaces->len)
            ws = g_ptr_array_index(workspaces, ws_idx);
        else
            ws = workspace_get_current();

        if (ws && cmd && cmd[0]) {
            GhosttyTerminal *term = resolve_target_terminal(ws, pane_idx, tab_idx, pane_id);
            if (term) {
                ghostty_surface_t surface = ghostty_terminal_get_surface(term);
                if (surface) {
                    ghostty_surface_text(surface, cmd, strlen(cmd));
                    ghostty_surface_text(surface, "\n", 1);
                    json_builder_set_member_name(response, "status");
                    json_builder_add_string_value(response, "ok");
                } else {
                    json_builder_set_member_name(response, "status");
                    json_builder_add_string_value(response, "error");
                }
            } else {
                json_builder_set_member_name(response, "status");
                json_builder_add_string_value(response, "error");
                json_builder_set_member_name(response, "message");
                json_builder_add_string_value(response, "no terminal found");
            }
        } else {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "error");
        }
    } else if (strcmp(command, "pane.read_text") == 0) {
        int ws_idx = (int)json_object_get_int_member_with_default(msg, "workspace", -1);
        int pane_idx = (int)json_object_get_int_member_with_default(msg, "pane", -1);
        int tab_idx = (int)json_object_get_int_member_with_default(msg, "tab", -1);
        int lines = (int)json_object_get_int_member_with_default(msg, "lines", 0);
        const char *pane_id =
            json_object_get_string_member_with_default(msg, "paneId", "");
        gboolean scrollback =
            json_object_get_boolean_member_with_default(msg, "scrollback", TRUE);
        Workspace *ws = NULL;

        if (ws_idx >= 0 && workspaces && ws_idx < (int)workspaces->len)
            ws = g_ptr_array_index(workspaces, ws_idx);
        else
            ws = workspace_get_current();

        if (ws) {
            GhosttyTerminal *term = resolve_target_terminal(ws, pane_idx, tab_idx, pane_id);
            if (term) {
                ghostty_surface_t surface = ghostty_terminal_get_surface(term);
                if (surface) {
                    ghostty_text_s captured = {0};
                    ghostty_selection_s sel = {
                        .top_left = {
                            .tag = scrollback ? GHOSTTY_POINT_SCREEN : GHOSTTY_POINT_VIEWPORT,
                            .coord = GHOSTTY_POINT_COORD_TOP_LEFT,
                            .x = 0,
                            .y = 0,
                        },
                        .bottom_right = {
                            .tag = scrollback ? GHOSTTY_POINT_SCREEN : GHOSTTY_POINT_VIEWPORT,
                            .coord = GHOSTTY_POINT_COORD_BOTTOM_RIGHT,
                            .x = 0,
                            .y = 0,
                        },
                        .rectangle = FALSE,
                    };

                    if (ghostty_surface_read_text(surface, sel, &captured) &&
                        captured.text) {
                        g_autofree char *trimmed =
                            prettymux_trim_last_lines(captured.text, lines);
                        json_builder_set_member_name(response, "status");
                        json_builder_add_string_value(response, "ok");
                        json_builder_set_member_name(response, "text");
                        json_builder_add_string_value(response, trimmed ? trimmed : "");
                        ghostty_surface_free_text(surface, &captured);
                    } else {
                        json_builder_set_member_name(response, "status");
                        json_builder_add_string_value(response, "error");
                        json_builder_set_member_name(response, "message");
                        json_builder_add_string_value(response, "read_text failed");
                    }
                } else {
                    json_builder_set_member_name(response, "status");
                    json_builder_add_string_value(response, "error");
                    json_builder_set_member_name(response, "message");
                    json_builder_add_string_value(response, "terminal has no surface");
                }
            } else {
                json_builder_set_member_name(response, "status");
                json_builder_add_string_value(response, "error");
                json_builder_set_member_name(response, "message");
                json_builder_add_string_value(response, "no terminal found");
            }
        } else {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "error");
            json_builder_set_member_name(response, "message");
            json_builder_add_string_value(response, "no terminal found");
        }
    } else if (strcmp(command, "port.report") == 0) {
        int port = (int)json_object_get_int_member_with_default(msg, "port", 0);
        const char *terminal_id =
            json_object_get_string_member_with_default(msg, "terminalId", "");

        if (port > 0 && terminal_id[0]) {
            terminal_routing_handle_reported_port(terminal_id, port);
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "ok");
        } else {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "error");
            json_builder_set_member_name(response, "message");
            json_builder_add_string_value(response, "missing port or terminalId");
        }
    } else if (strcmp(command, "terminal.register") == 0) {
        const char *terminal_id =
            json_object_get_string_member_with_default(msg, "terminalId", "");
        pid_t session_id = (pid_t)json_object_get_int_member_with_default(msg, "sessionId", 0);
        const char *tty_name =
            json_object_get_string_member_with_default(msg, "ttyName", "");
        const char *tty_path =
            json_object_get_string_member_with_default(msg, "ttyPath", "");

        terminal_routing_register_scope(terminal_id, session_id, tty_name, tty_path);
        json_builder_set_member_name(response, "status");
        json_builder_add_string_value(response, "ok");
    } else if (strcmp(command, "dismiss.welcome") == 0) {
        if (g_main_window)
            gtk_window_present(g_main_window);
        {
            char *dir = g_build_filename(g_get_home_dir(), ".config", "prettymux", NULL);
            g_mkdir_with_parents(dir, 0755);
            char *flag = g_build_filename(dir, ".welcome-shown", NULL);
            g_file_set_contents(flag, "1", 1, NULL);
            g_free(flag);
            g_free(dir);
        }
        json_builder_set_member_name(response, "status");
        json_builder_add_string_value(response, "ok");
    } else if (strcmp(command, "app.quit") == 0) {
        json_builder_set_member_name(response, "status");
        json_builder_add_string_value(response, "ok");
        app_actions_request_app_quit_async();
    } else if (strcmp(command, "list.actions") == 0) {
        json_builder_set_member_name(response, "status");
        json_builder_add_string_value(response, "ok");
        json_builder_set_member_name(response, "actions");
        json_builder_begin_array(response);
        for (int i = 0; default_shortcuts[i].action != NULL; i++)
            json_builder_add_string_value(response, default_shortcuts[i].action);
        json_builder_add_string_value(response, "browser.open");
        json_builder_add_string_value(response, "workspace.new");
        json_builder_add_string_value(response, "workspace.list");
        json_builder_add_string_value(response, "workspace.switch");
        json_builder_add_string_value(response, "workspace.import");
        json_builder_add_string_value(response, "workspace.move_to_instance");
        json_builder_add_string_value(response, "workspace.set_layout");
        json_builder_add_string_value(response, "workspace.get_layout");
        json_builder_add_string_value(response, "workspace.status.set");
        json_builder_add_string_value(response, "workspace.status.clear");
        json_builder_add_string_value(response, "workspace.status.list");
        json_builder_add_string_value(response, "tabs.list");
        json_builder_add_string_value(response, "tab.new");
        json_builder_add_string_value(response, "tab.move");
        json_builder_add_string_value(response, "tab.select");
        json_builder_add_string_value(response, "app.quit");
        json_builder_add_string_value(response, "exec");
        json_builder_add_string_value(response, "type");
        json_builder_end_array(response);
    } else {
        json_builder_set_member_name(response, "status");
        json_builder_add_string_value(response, "error");
        json_builder_set_member_name(response, "message");
        json_builder_add_string_value(response, "unknown command");
    }

    if (strcmp(command, "workspace.list") != 0 &&
        strcmp(command, "workspace.get_layout") != 0 &&
        strcmp(command, "workspace.get_strip_state") != 0 &&
        strcmp(command, "workspace.status.list") != 0 &&
        strcmp(command, "workspace.status.set") != 0 &&
        strcmp(command, "workspace.status.clear") != 0 &&
        strcmp(command, "tabs.list") != 0 &&
        strcmp(command, "list.actions") != 0 &&
        strcmp(command, "app.quit") != 0 &&
        strcmp(command, "tab.edit") != 0 &&
        strcmp(command, "workspace.edit") != 0 &&
        strcmp(command, "port.report") != 0) {
        session_queue_save();
    }
}
