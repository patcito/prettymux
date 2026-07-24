#include "agent_sessions.h"

#include <glib/gstdio.h>
#include <sqlite3.h>

static void
write_fixture(const char *path, const char *contents)
{
    g_autofree char *dir = g_path_get_dirname(path);

    g_assert_cmpint(g_mkdir_with_parents(dir, 0700), ==, 0);
    g_assert_true(g_file_set_contents(path, contents, -1, NULL));
}

static void
test_loads_claude_and_codex_and_applies_aliases(void)
{
    g_autofree char *home =
        g_dir_make_tmp("prettymux-agent-sessions-XXXXXX", NULL);
    g_autofree char *claude_path =
        g_build_filename(home, ".claude", "projects", "demo",
                         "claude-id.jsonl", NULL);
    g_autofree char *database_path =
        g_build_filename(home, ".codex", "state_5.sqlite", NULL);
    g_autofree char *aliases_path =
        g_build_filename(home, ".config", "prettymux",
                         "history-aliases.json", NULL);
    g_autofree char *database_dir = g_path_get_dirname(database_path);
    sqlite3 *database = NULL;
    GPtrArray *sessions;
    gboolean saw_claude = FALSE;
    gboolean saw_codex = FALSE;

    g_assert_nonnull(home);
    write_fixture(
        claude_path,
        "{\"type\":\"user\",\"cwd\":\"/projects/demo\","
        "\"message\":{\"content\":\"Claude first question\"}}\n");
    write_fixture(
        aliases_path,
        "{\"version\":1,\"titles\":{\"claude:claude-id\":"
        "\"Renamed Claude chat\"}}\n");

    g_assert_cmpint(g_mkdir_with_parents(database_dir, 0700), ==, 0);
    g_assert_cmpint(sqlite3_open(database_path, &database), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_exec(
        database,
        "CREATE TABLE threads ("
        "id TEXT, title TEXT, first_user_message TEXT, cwd TEXT,"
        "updated_at INTEGER, recency_at INTEGER, source TEXT,"
        "thread_source TEXT, agent_nickname TEXT, agent_role TEXT);"
        "INSERT INTO threads VALUES("
        "'codex-id','Codex first question','',"
        "'/projects/demo',200,200,'cli','user',NULL,NULL);",
        NULL, NULL, NULL), ==, SQLITE_OK);
    sqlite3_close(database);

    sessions = agent_sessions_load(home, 20);
    g_assert_cmpuint(sessions->len, ==, 2);
    for (guint i = 0; i < sessions->len; i++) {
        AgentSession *session = g_ptr_array_index(sessions, i);

        g_assert_cmpstr(session->cwd, ==, "/projects/demo");
        if (g_strcmp0(session->provider, "claude") == 0) {
            saw_claude = TRUE;
            g_assert_cmpstr(session->session_id, ==, "claude-id");
            g_assert_cmpstr(session->title, ==, "Renamed Claude chat");
        } else if (g_strcmp0(session->provider, "codex") == 0) {
            saw_codex = TRUE;
            g_assert_cmpstr(session->session_id, ==, "codex-id");
            g_assert_cmpstr(session->title, ==, "Codex first question");
        }
    }
    g_assert_true(saw_claude);
    g_assert_true(saw_codex);
    g_ptr_array_unref(sessions);
}

static void
test_long_utf8_title_stays_valid(void)
{
    g_autofree char *home =
        g_dir_make_tmp("prettymux-agent-utf8-XXXXXX", NULL);
    g_autofree char *claude_path =
        g_build_filename(home, ".claude", "projects", "demo",
                         "utf8-id.jsonl", NULL);
    g_autoptr(GString) title = g_string_new("");
    g_autofree char *record = NULL;
    GPtrArray *sessions;
    AgentSession *session;

    for (int i = 0; i < 160; i++)
        g_string_append(title, "中");
    record = g_strdup_printf(
        "{\"type\":\"user\",\"cwd\":\"/projects/utf8\","
        "\"message\":{\"content\":\"%s\"}}\n", title->str);
    write_fixture(claude_path, record);

    sessions = agent_sessions_load(home, 20);
    g_assert_cmpuint(sessions->len, ==, 1);
    session = g_ptr_array_index(sessions, 0);
    g_assert_true(g_utf8_validate(session->title, -1, NULL));
    g_assert_cmpint(g_utf8_strlen(session->title, -1), ==, 120);
    g_ptr_array_unref(sessions);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/agent-sessions/load/claude-codex-aliases",
                    test_loads_claude_and_codex_and_applies_aliases);
    g_test_add_func("/agent-sessions/load/utf8-title-limit",
                    test_long_utf8_title_stays_valid);
    return g_test_run();
}
