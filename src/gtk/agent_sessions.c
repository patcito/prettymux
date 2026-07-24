#include "agent_sessions.h"

#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#ifdef HAVE_SQLITE3
#include <sqlite3.h>
#endif
#include <string.h>
#include <unistd.h>

#include "app_state.h"
#include "ghostty_terminal.h"
#include "workspace.h"

#define AGENT_SESSION_TITLE_LIMIT 120

typedef struct {
    char *path;
    gint64 updated_at;
} ClaudeFile;

typedef struct {
    GtkWidget *panel;
    GtkWidget *groups_box;
    GPtrArray *sessions;
    GHashTable *aliases;
} AgentSessionsUi;

typedef struct {
    AgentSession *session;
    GtkWidget *resume_button;
    GtkWidget *title_label;
    GtkWidget *rename_button;
    GtkWidget *rename_entry;
    AgentSessionsUi *ui_state;
} AgentSessionRow;

typedef struct {
    guint64 workspace_serial;
    char *command;
    guint attempts;
} AgentSessionLaunch;

typedef struct {
    char *cwd;
    GPtrArray *sessions; /* borrowed AgentSession* */
} AgentSessionGroup;

static char *
single_line(const char *text)
{
    GString *out;
    gboolean pending_space = FALSE;
    guint character_count = 0;

    if (!text)
        return g_strdup("");

    out = g_string_sized_new(MIN(strlen(text), AGENT_SESSION_TITLE_LIMIT));
    for (const char *p = text;
         *p && character_count < AGENT_SESSION_TITLE_LIMIT;) {
        gunichar character = g_utf8_get_char_validated(p, -1);

        if (character == (gunichar)-1 || character == (gunichar)-2) {
            p++;
            continue;
        }
        p = g_utf8_next_char(p);
        if (g_unichar_isspace(character)) {
            pending_space = out->len > 0;
            continue;
        }
        if (pending_space && character_count < AGENT_SESSION_TITLE_LIMIT) {
            g_string_append_c(out, ' ');
            character_count++;
        }
        pending_space = FALSE;
        if (character_count < AGENT_SESSION_TITLE_LIMIT) {
            g_string_append_unichar(out, character);
            character_count++;
        }
    }
    g_strstrip(out->str);
    return g_string_free(out, FALSE);
}

void
agent_session_free(gpointer data)
{
    AgentSession *session = data;

    if (!session)
        return;
    g_free(session->provider);
    g_free(session->session_id);
    g_free(session->cwd);
    g_free(session->title);
    g_free(session);
}

static void
claude_file_free(gpointer data)
{
    ClaudeFile *file = data;

    if (!file)
        return;
    g_free(file->path);
    g_free(file);
}

static int
claude_file_compare(gconstpointer a, gconstpointer b)
{
    const ClaudeFile *left = *(ClaudeFile * const *)a;
    const ClaudeFile *right = *(ClaudeFile * const *)b;

    if (left->updated_at == right->updated_at)
        return g_strcmp0(left->path, right->path);
    return left->updated_at < right->updated_at ? 1 : -1;
}

static int
agent_session_compare(gconstpointer a, gconstpointer b)
{
    const AgentSession *left = *(AgentSession * const *)a;
    const AgentSession *right = *(AgentSession * const *)b;

    if (left->updated_at == right->updated_at)
        return g_strcmp0(left->title, right->title);
    return left->updated_at < right->updated_at ? 1 : -1;
}

static char *
claude_message_text(JsonObject *record)
{
    JsonObject *message;
    JsonNode *content;

    if (!record || !json_object_has_member(record, "message"))
        return g_strdup("");
    message = json_object_get_object_member(record, "message");
    if (!message || !json_object_has_member(message, "content"))
        return g_strdup("");

    content = json_object_get_member(message, "content");
    if (JSON_NODE_HOLDS_VALUE(content) &&
        json_node_get_value_type(content) == G_TYPE_STRING) {
        return single_line(json_node_get_string(content));
    }

    if (JSON_NODE_HOLDS_ARRAY(content)) {
        JsonArray *parts = json_node_get_array(content);
        GString *combined = g_string_new("");

        for (guint i = 0; i < json_array_get_length(parts); i++) {
            JsonNode *part_node = json_array_get_element(parts, i);
            JsonObject *part;
            const char *text;

            if (!part_node || !JSON_NODE_HOLDS_OBJECT(part_node))
                continue;
            part = json_node_get_object(part_node);
            text = json_object_get_string_member_with_default(part, "text", "");
            if (!text[0])
                continue;
            if (combined->len)
                g_string_append_c(combined, ' ');
            g_string_append(combined, text);
        }

        {
            g_autofree char *raw = g_string_free(combined, FALSE);
            return single_line(raw);
        }
    }

    return g_strdup("");
}

static AgentSession *
load_claude_session(const ClaudeFile *file)
{
    g_autoptr(GFile) source = NULL;
    g_autoptr(GFileInputStream) stream = NULL;
    g_autoptr(GDataInputStream) lines = NULL;
    g_autoptr(GError) error = NULL;
    AgentSession *session = NULL;

    source = g_file_new_for_path(file->path);
    stream = g_file_read(source, NULL, &error);
    if (!stream)
        return NULL;
    lines = g_data_input_stream_new(G_INPUT_STREAM(stream));

    for (;;) {
        g_autofree char *line =
            g_data_input_stream_read_line(lines, NULL, NULL, &error);
        g_autoptr(JsonParser) parser = NULL;
        JsonNode *root;
        JsonObject *record;
        const char *type;
        const char *cwd;

        if (!line)
            break;
        parser = json_parser_new();
        if (!json_parser_load_from_data(parser, line, -1, NULL))
            continue;
        root = json_parser_get_root(parser);
        if (!root || !JSON_NODE_HOLDS_OBJECT(root))
            continue;
        record = json_node_get_object(root);
        type = json_object_get_string_member_with_default(record, "type", "");
        if (g_strcmp0(type, "user") != 0)
            continue;
        if (json_object_get_boolean_member_with_default(
                record, "isSidechain", FALSE) ||
            json_object_has_member(record, "agentId")) {
            return NULL;
        }

        cwd = json_object_get_string_member_with_default(record, "cwd", "");
        if (!cwd[0])
            continue;

        session = g_new0(AgentSession, 1);
        session->provider = g_strdup("claude");
        session->cwd = g_strdup(cwd);
        session->title = claude_message_text(record);
        session->updated_at = file->updated_at;

        {
            g_autofree char *base = g_path_get_basename(file->path);
            char *suffix = g_strrstr(base, ".jsonl");
            if (suffix)
                *suffix = '\0';
            session->session_id = g_strdup(base);
        }
        if (!session->title[0]) {
            g_free(session->title);
            session->title = g_strdup("Untitled Claude chat");
        }
        return session;
    }

    return NULL;
}

static void
load_claude_sessions(GPtrArray *sessions,
                     const char *home_dir,
                     guint limit)
{
    g_autofree char *projects_path =
        g_build_filename(home_dir, ".claude", "projects", NULL);
    g_autoptr(GDir) projects = g_dir_open(projects_path, 0, NULL);
    g_autoptr(GPtrArray) files =
        g_ptr_array_new_with_free_func(claude_file_free);
    const char *project_name;
    guint loaded = 0;

    if (!projects)
        return;

    while ((project_name = g_dir_read_name(projects)) != NULL) {
        g_autofree char *project_path =
            g_build_filename(projects_path, project_name, NULL);
        g_autoptr(GDir) project = g_dir_open(project_path, 0, NULL);
        const char *file_name;

        if (!project)
            continue;
        while ((file_name = g_dir_read_name(project)) != NULL) {
            g_autofree char *file_path = NULL;
            GStatBuf stat_buf;
            ClaudeFile *file;

            if (!g_str_has_suffix(file_name, ".jsonl"))
                continue;
            file_path = g_build_filename(project_path, file_name, NULL);
            if (g_stat(file_path, &stat_buf) != 0)
                continue;
            file = g_new0(ClaudeFile, 1);
            file->path = g_steal_pointer(&file_path);
            file->updated_at = (gint64)stat_buf.st_mtime;
            g_ptr_array_add(files, file);
        }
    }

    g_ptr_array_sort(files, claude_file_compare);
    for (guint i = 0; i < files->len && loaded < limit; i++) {
        AgentSession *session =
            load_claude_session(g_ptr_array_index(files, i));
        if (!session)
            continue;
        g_ptr_array_add(sessions, session);
        loaded++;
    }
}

#ifdef HAVE_SQLITE3
static void
load_codex_sessions(GPtrArray *sessions,
                    const char *home_dir,
                    guint limit)
{
    g_autofree char *database_path =
        g_build_filename(home_dir, ".codex", "state_5.sqlite", NULL);
    sqlite3 *database = NULL;
    sqlite3_stmt *statement = NULL;
    static const char query[] =
        "SELECT id, "
        "COALESCE(NULLIF(title, ''), NULLIF(first_user_message, '')), "
        "cwd, MAX(COALESCE(NULLIF(recency_at, 0), updated_at), updated_at) "
        "FROM threads "
        "WHERE source = 'cli' "
        "AND (thread_source IS NULL OR thread_source = 'user') "
        "AND agent_nickname IS NULL "
        "AND agent_role IS NULL "
        "AND (title <> '' OR first_user_message <> '') "
        "ORDER BY MAX(COALESCE(NULLIF(recency_at, 0), updated_at), updated_at) "
        "DESC LIMIT ?";

    if (!g_file_test(database_path, G_FILE_TEST_IS_REGULAR))
        return;
    if (sqlite3_open_v2(database_path, &database,
                        SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, NULL)
        != SQLITE_OK) {
        goto out;
    }
    sqlite3_busy_timeout(database, 1000);
    if (sqlite3_prepare_v2(database, query, -1, &statement, NULL) != SQLITE_OK)
        goto out;
    sqlite3_bind_int(statement, 1, (int)limit);

    while (sqlite3_step(statement) == SQLITE_ROW) {
        const char *session_id =
            (const char *)sqlite3_column_text(statement, 0);
        const char *title =
            (const char *)sqlite3_column_text(statement, 1);
        const char *cwd =
            (const char *)sqlite3_column_text(statement, 2);
        AgentSession *session;

        if (!session_id || !session_id[0])
            continue;
        session = g_new0(AgentSession, 1);
        session->provider = g_strdup("codex");
        session->session_id = g_strdup(session_id);
        session->cwd = g_strdup((cwd && cwd[0]) ? cwd : home_dir);
        session->title = single_line(title);
        session->updated_at = sqlite3_column_int64(statement, 3);
        if (!session->title[0]) {
            g_free(session->title);
            session->title = g_strdup("Untitled Codex chat");
        }
        g_ptr_array_add(sessions, session);
    }

out:
    if (statement)
        sqlite3_finalize(statement);
    if (database)
        sqlite3_close(database);
}
#else
static void
load_codex_sessions(GPtrArray *sessions,
                    const char *home_dir,
                    guint limit)
{
    (void)sessions;
    (void)home_dir;
    (void)limit;
}
#endif

static char *
aliases_path(const char *home_dir)
{
    return g_build_filename(home_dir, ".config", "prettymux",
                            "history-aliases.json", NULL);
}

static GHashTable *
aliases_load(const char *home_dir)
{
    g_autofree char *path = aliases_path(home_dir);
    g_autoptr(JsonParser) parser = json_parser_new();
    GHashTable *aliases =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    JsonNode *root;
    JsonObject *root_object;
    JsonObject *titles;
    GList *members;

    if (!json_parser_load_from_file(parser, path, NULL))
        return aliases;
    root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root))
        return aliases;
    root_object = json_node_get_object(root);
    if (!json_object_has_member(root_object, "titles"))
        return aliases;
    titles = json_object_get_object_member(root_object, "titles");
    if (!titles)
        return aliases;

    members = json_object_get_members(titles);
    for (GList *item = members; item; item = item->next) {
        const char *key = item->data;
        const char *value =
            json_object_get_string_member_with_default(titles, key, "");
        if (value[0])
            g_hash_table_insert(aliases, g_strdup(key), g_strdup(value));
    }
    g_list_free(members);
    return aliases;
}

static void
apply_aliases(GPtrArray *sessions, GHashTable *aliases)
{
    for (guint i = 0; i < sessions->len; i++) {
        AgentSession *session = g_ptr_array_index(sessions, i);
        g_autofree char *key =
            g_strdup_printf("%s:%s", session->provider, session->session_id);
        const char *alias = g_hash_table_lookup(aliases, key);

        if (!alias || !alias[0])
            continue;
        g_free(session->title);
        session->title = g_strdup(alias);
    }
}

GPtrArray *
agent_sessions_load(const char *home_dir, guint per_provider_limit)
{
    GPtrArray *sessions =
        g_ptr_array_new_with_free_func(agent_session_free);
    g_autoptr(GHashTable) aliases = NULL;
    const char *home =
        (home_dir && home_dir[0]) ? home_dir : g_get_home_dir();
    guint limit = CLAMP(per_provider_limit, 1, 50);

    load_claude_sessions(sessions, home, limit);
    load_codex_sessions(sessions, home, limit);
    g_ptr_array_sort(sessions, agent_session_compare);
    aliases = aliases_load(home);
    apply_aliases(sessions, aliases);
    return sessions;
}

static gboolean
aliases_save(GHashTable *aliases)
{
    g_autofree char *path = aliases_path(g_get_home_dir());
    g_autofree char *dir = g_path_get_dirname(path);
    g_autofree char *template_path = NULL;
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(JsonGenerator) generator = json_generator_new();
    g_autoptr(JsonNode) root = NULL;
    g_autofree char *data = NULL;
    gsize data_len = 0;
    GHashTableIter iter;
    gpointer key;
    gpointer value;
    int fd;
    gboolean ok = FALSE;

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "version");
    json_builder_add_int_value(builder, 1);
    json_builder_set_member_name(builder, "titles");
    json_builder_begin_object(builder);
    g_hash_table_iter_init(&iter, aliases);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        json_builder_set_member_name(builder, key);
        json_builder_add_string_value(builder, value);
    }
    json_builder_end_object(builder);
    json_builder_end_object(builder);

    root = json_builder_get_root(builder);
    json_generator_set_root(generator, root);
    json_generator_set_pretty(generator, TRUE);
    data = json_generator_to_data(generator, &data_len);

    if (g_mkdir_with_parents(dir, 0700) != 0 && errno != EEXIST)
        return FALSE;
    template_path = g_strdup_printf("%s.XXXXXX", path);
    fd = g_mkstemp_full(template_path, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0)
        return FALSE;
    if (write(fd, data, data_len) == (ssize_t)data_len &&
        write(fd, "\n", 1) == 1 &&
        fsync(fd) == 0 &&
        close(fd) == 0) {
        fd = -1;
        ok = g_rename(template_path, path) == 0;
    }
    if (fd >= 0)
        close(fd);
    if (!ok)
        g_unlink(template_path);
    return ok;
}

static char *
relative_age(gint64 timestamp)
{
    gint64 seconds = MAX((gint64)0, g_get_real_time() / G_USEC_PER_SEC
                                      - timestamp);

    if (seconds < 60)
        return g_strdup("now");
    if (seconds < 3600)
        return g_strdup_printf("%" G_GINT64_FORMAT "m ago", seconds / 60);
    if (seconds < 86400)
        return g_strdup_printf("%" G_GINT64_FORMAT "h ago", seconds / 3600);
    return g_strdup_printf("%" G_GINT64_FORMAT "d ago", seconds / 86400);
}

static Workspace *
workspace_for_serial(guint64 serial)
{
    if (!workspaces)
        return NULL;
    for (guint i = 0; i < workspaces->len; i++) {
        Workspace *ws = g_ptr_array_index(workspaces, i);
        if (ws && ws->serial == serial)
            return ws;
    }
    return NULL;
}

static gboolean
send_resume_command(gpointer user_data)
{
    AgentSessionLaunch *launch = user_data;
    Workspace *ws = workspace_for_serial(launch->workspace_serial);
    GtkNotebook *notebook;
    GtkWidget *page;
    GtkWidget *terminal;
    ghostty_surface_t surface;
    int page_num;

    if (!ws)
        goto done;
    notebook = workspace_get_focused_pane(ws);
    page_num = GTK_IS_NOTEBOOK(notebook)
        ? gtk_notebook_get_current_page(notebook) : -1;
    page = page_num >= 0
        ? gtk_notebook_get_nth_page(notebook, page_num) : NULL;
    terminal = page
        ? g_object_get_data(G_OBJECT(page), "linked-terminal") : NULL;
    surface = GHOSTTY_IS_TERMINAL(terminal)
        ? ghostty_terminal_get_surface(GHOSTTY_TERMINAL(terminal)) : NULL;

    if (!surface && ++launch->attempts < 30)
        return G_SOURCE_CONTINUE;
    if (surface) {
        ghostty_surface_text(surface, launch->command,
                            strlen(launch->command));
        ghostty_surface_text(surface, "\n", 1);
        ghostty_terminal_focus(GHOSTTY_TERMINAL(terminal));
    }

done:
    g_free(launch->command);
    g_free(launch);
    return G_SOURCE_REMOVE;
}

static void
resume_session(GtkButton *button, gpointer user_data)
{
    AgentSessionRow *row = user_data;
    AgentSessionLaunch *launch;
    Workspace *ws;
    g_autofree char *quoted_id = NULL;

    (void)button;
    if (!row || !row->session)
        return;

    workspace_add_with_cwd(ui.terminal_stack, ui.workspace_list,
                           g_ghostty_app, row->session->cwd);
    ws = workspace_get_current();
    if (!ws)
        return;

    quoted_id = g_shell_quote(row->session->session_id);
    launch = g_new0(AgentSessionLaunch, 1);
    launch->workspace_serial = ws->serial;
    launch->command = g_strdup_printf(
        g_strcmp0(row->session->provider, "claude") == 0
            ? "claude --resume %s" : "codex resume %s",
        quoted_id);
    g_timeout_add(100, send_resume_command, launch);
}

static void
finish_rename(AgentSessionRow *row)
{
    gtk_widget_set_visible(row->rename_entry, FALSE);
    gtk_widget_set_visible(row->resume_button, TRUE);
    gtk_widget_set_visible(row->rename_button, TRUE);
}

static void
commit_rename(AgentSessionRow *row)
{
    g_autofree char *title =
        single_line(gtk_editable_get_text(GTK_EDITABLE(row->rename_entry)));
    g_autofree char *key = NULL;

    if (!title[0]) {
        finish_rename(row);
        return;
    }

    g_free(row->session->title);
    row->session->title = g_strdup(title);
    gtk_label_set_text(GTK_LABEL(row->title_label), title);
    key = g_strdup_printf("%s:%s", row->session->provider,
                          row->session->session_id);
    g_hash_table_replace(row->ui_state->aliases,
                         g_strdup(key), g_strdup(title));
    aliases_save(row->ui_state->aliases);
    finish_rename(row);
}

static void
rename_activated(GtkEntry *entry, gpointer user_data)
{
    (void)entry;
    commit_rename(user_data);
}

static gboolean
rename_key_pressed(GtkEventControllerKey *controller,
                   guint keyval,
                   guint keycode,
                   GdkModifierType state,
                   gpointer user_data)
{
    (void)controller;
    (void)keycode;
    (void)state;

    if (keyval == GDK_KEY_Escape) {
        finish_rename(user_data);
        return TRUE;
    }
    return FALSE;
}

static void
start_rename(GtkButton *button, gpointer user_data)
{
    AgentSessionRow *row = user_data;

    (void)button;
    gtk_editable_set_text(GTK_EDITABLE(row->rename_entry),
                          row->session->title);
    gtk_widget_set_visible(row->resume_button, FALSE);
    gtk_widget_set_visible(row->rename_button, FALSE);
    gtk_widget_set_visible(row->rename_entry, TRUE);
    gtk_widget_grab_focus(row->rename_entry);
    gtk_editable_select_region(GTK_EDITABLE(row->rename_entry), 0, -1);
}

static void
agent_session_row_free(gpointer data)
{
    g_free(data);
}

static GtkWidget *
session_row_new(AgentSessionsUi *sessions_ui, AgentSession *session)
{
    GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    GtkWidget *resume = gtk_button_new();
    GtkWidget *title = gtk_label_new(session->title);
    GtkWidget *meta;
    GtkWidget *rename =
        gtk_button_new_from_icon_name("document-edit-symbolic");
    GtkWidget *entry = gtk_entry_new();
    GtkEventController *keys = gtk_event_controller_key_new();
    AgentSessionRow *row = g_new0(AgentSessionRow, 1);
    g_autofree char *age = relative_age(session->updated_at);
    g_autofree char *meta_text =
        g_strdup_printf("%s · %s", session->provider, age);

    row->session = session;
    row->resume_button = resume;
    row->title_label = title;
    row->rename_button = rename;
    row->rename_entry = entry;
    row->ui_state = sessions_ui;

    gtk_label_set_xalign(GTK_LABEL(title), 0);
    gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
    meta = gtk_label_new(meta_text);
    gtk_label_set_xalign(GTK_LABEL(meta), 0);
    gtk_widget_add_css_class(meta, "dim-label");
    gtk_box_append(GTK_BOX(content), title);
    gtk_box_append(GTK_BOX(content), meta);
    gtk_button_set_child(GTK_BUTTON(resume), content);
    gtk_widget_set_hexpand(resume, TRUE);
    gtk_widget_set_tooltip_text(resume, session->cwd);

    gtk_button_set_has_frame(GTK_BUTTON(rename), FALSE);
    gtk_widget_set_tooltip_text(rename,
                                "Rename chat (PrettyMux display only)");
    gtk_widget_set_visible(entry, FALSE);
    gtk_widget_set_hexpand(entry, TRUE);

    g_signal_connect(resume, "clicked", G_CALLBACK(resume_session), row);
    g_signal_connect(rename, "clicked", G_CALLBACK(start_rename), row);
    g_signal_connect(entry, "activate", G_CALLBACK(rename_activated), row);
    g_signal_connect(keys, "key-pressed",
                     G_CALLBACK(rename_key_pressed), row);
    gtk_widget_add_controller(entry, keys);

    gtk_box_append(GTK_BOX(row_box), resume);
    gtk_box_append(GTK_BOX(row_box), entry);
    gtk_box_append(GTK_BOX(row_box), rename);
    g_object_set_data_full(G_OBJECT(row_box), "agent-session-row",
                           row, agent_session_row_free);
    return row_box;
}

static void
agent_session_group_free(gpointer data)
{
    AgentSessionGroup *group = data;

    if (!group)
        return;
    g_free(group->cwd);
    g_ptr_array_unref(group->sessions);
    g_free(group);
}

static GPtrArray *
group_sessions(GPtrArray *sessions)
{
    GPtrArray *groups =
        g_ptr_array_new_with_free_func(agent_session_group_free);
    GHashTable *by_path =
        g_hash_table_new(g_str_hash, g_str_equal);

    for (guint i = 0; i < sessions->len; i++) {
        AgentSession *session = g_ptr_array_index(sessions, i);
        AgentSessionGroup *group =
            g_hash_table_lookup(by_path, session->cwd);

        if (!group) {
            group = g_new0(AgentSessionGroup, 1);
            group->cwd = g_strdup(session->cwd);
            group->sessions = g_ptr_array_new();
            g_ptr_array_add(groups, group);
            g_hash_table_insert(by_path, group->cwd, group);
        }
        g_ptr_array_add(group->sessions, session);
    }

    g_hash_table_unref(by_path);
    return groups;
}

static void
clear_box(GtkWidget *box)
{
    GtkWidget *child;

    while ((child = gtk_widget_get_first_child(box)) != NULL)
        gtk_box_remove(GTK_BOX(box), child);
}

void
agent_sessions_panel_refresh(GtkWidget *panel)
{
    AgentSessionsUi *sessions_ui =
        g_object_get_data(G_OBJECT(panel), "agent-sessions-ui");
    g_autoptr(GPtrArray) groups = NULL;

    if (!sessions_ui)
        return;
    clear_box(sessions_ui->groups_box);
    g_clear_pointer(&sessions_ui->sessions, g_ptr_array_unref);
    g_clear_pointer(&sessions_ui->aliases, g_hash_table_unref);

    sessions_ui->sessions = agent_sessions_load(g_get_home_dir(), 20);
    sessions_ui->aliases = aliases_load(g_get_home_dir());
    if (sessions_ui->sessions->len == 0) {
        GtkWidget *empty = gtk_label_new("No resumable Claude/Codex chats");
        gtk_widget_add_css_class(empty, "dim-label");
        gtk_box_append(GTK_BOX(sessions_ui->groups_box), empty);
        return;
    }

    groups = group_sessions(sessions_ui->sessions);
    for (guint i = 0; i < groups->len; i++) {
        AgentSessionGroup *group = g_ptr_array_index(groups, i);
        GtkWidget *expander = gtk_expander_new(NULL);
        GtkWidget *path_label = gtk_label_new(group->cwd);
        GtkWidget *sessions_box =
            gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

        gtk_label_set_xalign(GTK_LABEL(path_label), 0);
        gtk_label_set_ellipsize(GTK_LABEL(path_label),
                                PANGO_ELLIPSIZE_MIDDLE);
        gtk_label_set_max_width_chars(GTK_LABEL(path_label), 24);
        gtk_widget_set_hexpand(path_label, TRUE);
        gtk_expander_set_label_widget(GTK_EXPANDER(expander), path_label);
        gtk_widget_set_tooltip_text(expander, group->cwd);
        gtk_expander_set_expanded(GTK_EXPANDER(expander), i == 0);
        for (guint j = 0; j < group->sessions->len; j++) {
            AgentSession *session =
                g_ptr_array_index(group->sessions, j);
            gtk_box_append(GTK_BOX(sessions_box),
                           session_row_new(sessions_ui, session));
        }
        gtk_expander_set_child(GTK_EXPANDER(expander), sessions_box);
        gtk_box_append(GTK_BOX(sessions_ui->groups_box), expander);
    }
}

static void
refresh_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    agent_sessions_panel_refresh(user_data);
}

static void
agent_sessions_ui_free(gpointer data)
{
    AgentSessionsUi *sessions_ui = data;

    if (!sessions_ui)
        return;
    g_clear_pointer(&sessions_ui->sessions, g_ptr_array_unref);
    g_clear_pointer(&sessions_ui->aliases, g_hash_table_unref);
    g_free(sessions_ui);
}

GtkWidget *
agent_sessions_panel_new(void)
{
    GtkWidget *panel = gtk_expander_new(NULL);
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *title = gtk_label_new("Claude / Codex Chats");
    GtkWidget *refresh =
        gtk_button_new_from_icon_name("view-refresh-symbolic");
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *groups_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    AgentSessionsUi *sessions_ui = g_new0(AgentSessionsUi, 1);

    gtk_widget_set_hexpand(title, TRUE);
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    gtk_widget_add_css_class(title, "heading");
    gtk_button_set_has_frame(GTK_BUTTON(refresh), FALSE);
    gtk_widget_set_tooltip_text(refresh, "Refresh Claude/Codex chats");
    gtk_box_append(GTK_BOX(header), title);
    gtk_box_append(GTK_BOX(header), refresh);
    gtk_expander_set_label_widget(GTK_EXPANDER(panel), header);

    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, -1, 280);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), groups_box);
    gtk_expander_set_child(GTK_EXPANDER(panel), scroll);
    gtk_widget_set_margin_start(panel, 8);
    gtk_widget_set_margin_end(panel, 8);
    gtk_widget_set_margin_bottom(panel, 4);

    sessions_ui->panel = panel;
    sessions_ui->groups_box = groups_box;
    g_object_set_data_full(G_OBJECT(panel), "agent-sessions-ui",
                           sessions_ui, agent_sessions_ui_free);
    g_object_set_data(G_OBJECT(panel), "agent-sessions-title", title);
    g_object_set_data(G_OBJECT(panel), "agent-sessions-refresh", refresh);
    g_signal_connect(refresh, "clicked", G_CALLBACK(refresh_clicked), panel);
    agent_sessions_panel_refresh(panel);
    return panel;
}
