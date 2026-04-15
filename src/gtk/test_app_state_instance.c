#include "app_state.h"

#include <glib.h>
#include <glib/gstdio.h>
#ifndef G_OS_WIN32
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

static gboolean
instance_id_char_allowed(char c)
{
    return g_ascii_isalnum(c) || c == '-' || c == '_' || c == '.';
}

static void
assert_instance_id_valid(const char *instance_id)
{
    g_assert_nonnull(instance_id);
    g_assert_true(instance_id[0] != '\0');
    for (const char *p = instance_id; *p; p++)
        g_assert_true(instance_id_char_allowed(*p));
}

static void
reset_instance_id(void)
{
    app_state()->instance_id[0] = '\0';
}

static void
clear_instance_env(void)
{
    g_unsetenv("PRETTYMUX");
    g_unsetenv("PRETTYMUX_SOCKET");
    g_unsetenv("PRETTYMUX_INSTANCE_ID");
    g_unsetenv("PRETTYMUX_CHILD_INSTANCE_ID");
    g_unsetenv("PRETTYMUX_TERMINAL_ID");
}

static void
test_default_instance_id_is_stable(void)
{
    const char *first;
    const char *second;

    reset_instance_id();
    first = app_state_get_instance_id();
    second = app_state_get_instance_id();

    assert_instance_id_valid(first);
    g_assert_cmpstr(first, ==, second);
}

static void
test_init_instance_id_from_env(void)
{
    reset_instance_id();
    g_unsetenv("PRETTYMUX");
    g_setenv("PRETTYMUX_INSTANCE_ID", "phase6!target@A-_.9", TRUE);
    app_state_init_instance_id_from_env();
    g_assert_cmpstr(app_state_get_instance_id(), ==, "phase6targetA-_.9");
    clear_instance_env();
}

static void
test_set_instance_id_sanitizes_value(void)
{
    app_state_set_instance_id("phase6!target@A-_.9");
    g_assert_cmpstr(app_state_get_instance_id(), ==, "phase6targetA-_.9");
}

static void
test_set_instance_id_invalid_value_falls_back(void)
{
    app_state_set_instance_id("!!!");
    assert_instance_id_valid(app_state_get_instance_id());
}

static void
test_init_instance_id_from_inside_prettymux_uses_child_id(void)
{
    reset_instance_id();
    g_setenv("PRETTYMUX", "1", TRUE);
    g_setenv("PRETTYMUX_SOCKET", "/tmp/prettymux-phase6-parent.sock", TRUE);
    g_setenv("PRETTYMUX_INSTANCE_ID", "phase6-parent", TRUE);
    g_setenv("PRETTYMUX_TERMINAL_ID", "term-7", TRUE);
    app_state_init_instance_id_from_env();
    g_assert_cmpstr(app_state_get_instance_id(), ==,
                    "phase6-parent-child-term-7");
    clear_instance_env();
}

static void
test_init_instance_id_from_inside_prettymux_sanitizes_parent(void)
{
    reset_instance_id();
    g_setenv("PRETTYMUX", "1", TRUE);
    g_setenv("PRETTYMUX_SOCKET", "/tmp/prettymux-phase6-parent.sock", TRUE);
    g_setenv("PRETTYMUX_INSTANCE_ID", "phase6!parent@", TRUE);
    g_setenv("PRETTYMUX_TERMINAL_ID", "term.9", TRUE);
    app_state_init_instance_id_from_env();
    g_assert_cmpstr(app_state_get_instance_id(), ==,
                    "phase6parent-child-term.9");
    clear_instance_env();
}

static void
test_nested_instance_id_is_restart_stable_per_terminal_lane(void)
{
    const char *first;
    const char *second;

    g_setenv("PRETTYMUX", "1", TRUE);
    g_setenv("PRETTYMUX_SOCKET", "/tmp/prettymux-phase6-parent.sock", TRUE);
    g_setenv("PRETTYMUX_INSTANCE_ID", "phase6-parent", TRUE);
    g_setenv("PRETTYMUX_TERMINAL_ID", "lane-a", TRUE);

    reset_instance_id();
    app_state_init_instance_id_from_env();
    first = g_strdup(app_state_get_instance_id());
    g_assert_cmpstr(first, ==, "phase6-parent-child-lane-a");

    reset_instance_id();
    app_state_init_instance_id_from_env();
    second = app_state_get_instance_id();
    g_assert_cmpstr(first, ==, second);

    g_free((char *)first);
    clear_instance_env();
}

static void
test_nested_instance_ids_are_distinct_for_different_terminal_lanes(void)
{
    const char *lane_a_id;
    const char *lane_b_id;

    g_setenv("PRETTYMUX", "1", TRUE);
    g_setenv("PRETTYMUX_SOCKET", "/tmp/prettymux-phase6-parent.sock", TRUE);
    g_setenv("PRETTYMUX_INSTANCE_ID", "phase6-parent", TRUE);

    g_setenv("PRETTYMUX_TERMINAL_ID", "lane-a", TRUE);
    reset_instance_id();
    app_state_init_instance_id_from_env();
    lane_a_id = g_strdup(app_state_get_instance_id());

    g_setenv("PRETTYMUX_TERMINAL_ID", "lane-b", TRUE);
    reset_instance_id();
    app_state_init_instance_id_from_env();
    lane_b_id = app_state_get_instance_id();

    g_assert_cmpstr(lane_a_id, !=, lane_b_id);
    g_assert_true(g_str_has_prefix(lane_a_id, "phase6-parent-child-lane-a"));
    g_assert_true(g_str_has_prefix(lane_b_id, "phase6-parent-child-lane-b"));

    g_free((char *)lane_a_id);
    clear_instance_env();
}

#ifndef G_OS_WIN32
typedef struct {
    int fd;
    char path[256];
} TestListeningSocket;

static char *
test_session_path_for_instance(const char *instance_id)
{
    g_autofree char *sessions_dir = g_build_filename(g_get_home_dir(),
                                                     ".prettymux",
                                                     "sessions",
                                                     NULL);
    g_autofree char *file_name = NULL;

    if (!instance_id || !instance_id[0] ||
        g_strcmp0(instance_id, "default") == 0) {
        file_name = g_strdup("last-default.json");
    } else {
        file_name = g_strdup_printf("last-%s.json", instance_id);
    }

    g_mkdir_with_parents(sessions_dir, 0755);
    return g_build_filename(sessions_dir, file_name, NULL);
}

static void
test_create_session_file_for_instance(const char *instance_id)
{
    g_autofree char *path = test_session_path_for_instance(instance_id);
    g_assert_true(g_file_set_contents(path, "{\"version\":1}", -1, NULL));
}

static void
test_remove_session_file_for_instance(const char *instance_id)
{
    g_autofree char *path = test_session_path_for_instance(instance_id);
    g_remove(path);
}

static gboolean
test_listening_socket_open(TestListeningSocket *sock, const char *instance_id)
{
    struct sockaddr_un addr = {0};

    g_assert_nonnull(sock);
    g_assert_nonnull(instance_id);
    g_assert_true(instance_id[0] != '\0');

    sock->fd = -1;
    g_snprintf(sock->path, sizeof(sock->path), "/tmp/prettymux-%s.sock",
               instance_id);
    g_unlink(sock->path);

    sock->fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock->fd < 0)
        return FALSE;

    addr.sun_family = AF_UNIX;
    g_strlcpy(addr.sun_path, sock->path, sizeof(addr.sun_path));
    if (bind(sock->fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(sock->fd);
        sock->fd = -1;
        return FALSE;
    }
    if (listen(sock->fd, 1) != 0) {
        close(sock->fd);
        sock->fd = -1;
        g_unlink(sock->path);
        sock->path[0] = '\0';
        return FALSE;
    }
    return TRUE;
}

static void
test_listening_socket_close(TestListeningSocket *sock)
{
    if (!sock)
        return;

    if (sock->fd >= 0) {
        close(sock->fd);
        sock->fd = -1;
    }
    if (sock->path[0])
        g_unlink(sock->path);
    sock->path[0] = '\0';
}

static void
test_nested_same_lane_child_slots_are_restart_stable_and_distinct(void)
{
    char *slot_a_first;
    const char *slot_a_second;
    char *slot_b_first;
    const char *slot_b_second;

    g_setenv("PRETTYMUX", "1", TRUE);
    g_setenv("PRETTYMUX_SOCKET", "/tmp/prettymux-phase6-parent.sock", TRUE);
    g_setenv("PRETTYMUX_INSTANCE_ID", "phase6-parent", TRUE);
    g_setenv("PRETTYMUX_TERMINAL_ID", "lane-a", TRUE);

    g_setenv("PRETTYMUX_CHILD_INSTANCE_ID", "lane-a-slot-1", TRUE);
    reset_instance_id();
    app_state_init_instance_id_from_env();
    slot_a_first = g_strdup(app_state_get_instance_id());
    g_assert_cmpstr(slot_a_first, ==, "phase6-parent-child-lane-a-slot-1");

    reset_instance_id();
    app_state_init_instance_id_from_env();
    slot_a_second = app_state_get_instance_id();
    g_assert_cmpstr(slot_a_first, ==, slot_a_second);

    g_setenv("PRETTYMUX_CHILD_INSTANCE_ID", "lane-a-slot-2", TRUE);
    reset_instance_id();
    app_state_init_instance_id_from_env();
    slot_b_first = g_strdup(app_state_get_instance_id());
    g_assert_cmpstr(slot_b_first, ==, "phase6-parent-child-lane-a-slot-2");

    reset_instance_id();
    app_state_init_instance_id_from_env();
    slot_b_second = app_state_get_instance_id();
    g_assert_cmpstr(slot_b_first, ==, slot_b_second);

    g_assert_cmpstr(slot_a_first, !=, slot_b_first);

    g_free(slot_a_first);
    g_free(slot_b_first);
    clear_instance_env();
}

static void
test_nested_same_lane_children_are_collision_free_without_explicit_slot(void)
{
    TestListeningSocket blocker = {0};
    g_autofree char *first_id = NULL;
    g_autofree char *second_id = NULL;
    const char *second_restart_id;

    g_setenv("PRETTYMUX", "1", TRUE);
    g_setenv("PRETTYMUX_SOCKET", "/tmp/prettymux-phase6-parent.sock", TRUE);
    g_setenv("PRETTYMUX_INSTANCE_ID", "phase6-parent", TRUE);
    g_setenv("PRETTYMUX_TERMINAL_ID", "lane-a", TRUE);
    g_unsetenv("PRETTYMUX_CHILD_INSTANCE_ID");

    reset_instance_id();
    app_state_init_instance_id_from_env();
    first_id = g_strdup(app_state_get_instance_id());
    g_assert_cmpstr(first_id, ==, "phase6-parent-child-lane-a");

    g_assert_true(test_listening_socket_open(&blocker, first_id));

    reset_instance_id();
    app_state_init_instance_id_from_env();
    second_id = g_strdup(app_state_get_instance_id());
    g_assert_cmpstr(second_id, ==, "phase6-parent-child-lane-a-2");
    g_assert_cmpstr(first_id, !=, second_id);

    test_create_session_file_for_instance(second_id);

    reset_instance_id();
    app_state_init_instance_id_from_env();
    second_restart_id = app_state_get_instance_id();
    g_assert_cmpstr(second_restart_id, ==, second_id);

    test_remove_session_file_for_instance(second_id);
    test_listening_socket_close(&blocker);
    clear_instance_env();
}

static void
test_nested_instance_id_does_not_mutate_from_live_socket_occupancy(void)
{
    TestListeningSocket blocker = {0};
    const char *expected = "phase6-parent-child-lane-a-slot-1";

    g_setenv("PRETTYMUX", "1", TRUE);
    g_setenv("PRETTYMUX_SOCKET", "/tmp/prettymux-phase6-parent.sock", TRUE);
    g_setenv("PRETTYMUX_INSTANCE_ID", "phase6-parent", TRUE);
    g_setenv("PRETTYMUX_TERMINAL_ID", "lane-a", TRUE);
    g_setenv("PRETTYMUX_CHILD_INSTANCE_ID", "lane-a-slot-1", TRUE);

    g_assert_true(test_listening_socket_open(&blocker, expected));

    reset_instance_id();
    app_state_init_instance_id_from_env();
    g_assert_cmpstr(app_state_get_instance_id(), ==, expected);

    test_listening_socket_close(&blocker);
    clear_instance_env();
}
#endif

int
main(int argc, char **argv)
{
    g_autofree char *tmp_home = NULL;

    g_test_init(&argc, &argv, NULL);
    tmp_home = g_dir_make_tmp("prettymux-app-state-instance-XXXXXX", NULL);
    g_assert_nonnull(tmp_home);
    g_setenv("HOME", tmp_home, TRUE);

    g_test_add_func("/app-state/instance/default-stable",
                    test_default_instance_id_is_stable);
    g_test_add_func("/app-state/instance/init-from-env",
                    test_init_instance_id_from_env);
    g_test_add_func("/app-state/instance/sanitize",
                    test_set_instance_id_sanitizes_value);
    g_test_add_func("/app-state/instance/invalid-fallback",
                    test_set_instance_id_invalid_value_falls_back);
    g_test_add_func("/app-state/instance/init-from-inside-prettymux-child",
                    test_init_instance_id_from_inside_prettymux_uses_child_id);
    g_test_add_func("/app-state/instance/init-from-inside-prettymux-sanitize",
                    test_init_instance_id_from_inside_prettymux_sanitizes_parent);
    g_test_add_func("/app-state/instance/nested-restart-stable-lane",
                    test_nested_instance_id_is_restart_stable_per_terminal_lane);
    g_test_add_func("/app-state/instance/nested-lanes-distinct",
                    test_nested_instance_ids_are_distinct_for_different_terminal_lanes);
#ifndef G_OS_WIN32
    g_test_add_func("/app-state/instance/nested-same-lane-slots-stable",
                    test_nested_same_lane_child_slots_are_restart_stable_and_distinct);
    g_test_add_func("/app-state/instance/nested-same-lane-default-auto-slot",
                    test_nested_same_lane_children_are_collision_free_without_explicit_slot);
    g_test_add_func("/app-state/instance/nested-no-occupancy-mutation",
                    test_nested_instance_id_does_not_mutate_from_live_socket_occupancy);
#endif

    return g_test_run();
}
