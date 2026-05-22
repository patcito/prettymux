#include "sidebar_sections.h"
#include "sidebar_ui.h"

#include <gtk/gtk.h>
#include <string.h>

#include "app_state.h"
#include "notifications.h"
#include "session.h"
#include "workspace.h"

static int workspace_switch_call_count = 0;
static int workspace_switch_last_index = -1;
static int session_queue_save_call_count = 0;
static int move_to_window_modal_call_count = 0;
static GtkOverlay *move_to_window_last_overlay = NULL;
static GtkWidget *move_to_window_last_terminal_stack = NULL;
static GtkWidget *move_to_window_last_workspace_list = NULL;
static int move_to_window_last_workspace_index = -1;

/* ---- Stubs required by sidebar_ui.c ---- */

AppState *
app_state(void)
{
    static AppState state;
    return &state;
}

void
workspace_switch(int index, GtkWidget *terminal_stack, GtkWidget *workspace_list)
{
    (void)index;
    (void)terminal_stack;
    (void)workspace_list;
    workspace_switch_call_count++;
    workspace_switch_last_index = index;
}

void
workspace_add(GtkWidget *terminal_stack, GtkWidget *workspace_list, ghostty_app_t app)
{
    (void)terminal_stack;
    (void)workspace_list;
    (void)app;
}

int
workspace_get_index(Workspace *ws)
{
    return ws ? 0 : -1;
}

void
session_queue_save(void)
{
    session_queue_save_call_count++;
}

void
notifications_on_bell_button_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    (void)user_data;
}

void
pane_move_overlay_toggle_workspace_targets(GtkOverlay *overlay,
                                           GtkWidget *terminal_stack,
                                           GtkWidget *workspace_list,
                                           int source_workspace_idx)
{
    move_to_window_modal_call_count++;
    move_to_window_last_overlay = overlay;
    move_to_window_last_terminal_stack = terminal_stack;
    move_to_window_last_workspace_list = workspace_list;
    move_to_window_last_workspace_index = source_workspace_idx;
}

/* ---- Test helpers ---- */

static gboolean have_display = FALSE;
static void
reset_stub_counters(void)
{
    sidebar_ui_cancel_pending_activate();
    workspace_switch_call_count = 0;
    workspace_switch_last_index = -1;
    session_queue_save_call_count = 0;
    move_to_window_modal_call_count = 0;
    move_to_window_last_overlay = NULL;
    move_to_window_last_terminal_stack = NULL;
    move_to_window_last_workspace_list = NULL;
    move_to_window_last_workspace_index = -1;
}

static void
require_display_or_skip(void)
{
    if (!have_display)
        g_test_skip("No GTK display available");
}

static gboolean
wait_for_workspace_switch_count(int expected)
{
    gint64 deadline = g_get_monotonic_time() + G_TIME_SPAN_SECOND;

    while (g_get_monotonic_time() < deadline) {
        while (g_main_context_iteration(NULL, FALSE))
            ;
        if (workspace_switch_call_count >= expected)
            return TRUE;
        g_usleep(1000);
    }

    while (g_main_context_iteration(NULL, FALSE))
        ;
    return workspace_switch_call_count >= expected;
}

static int
box_child_count(GtkWidget *box)
{
    GtkWidget *child;
    int count = 0;

    for (child = gtk_widget_get_first_child(box);
         child;
         child = gtk_widget_get_next_sibling(child)) {
        count++;
    }

    return count;
}

static GtkWidget *
box_child_at(GtkWidget *box, int index)
{
    GtkWidget *child;
    int i = 0;

    for (child = gtk_widget_get_first_child(box);
         child;
         child = gtk_widget_get_next_sibling(child)) {
        if (i == index)
            return child;
        i++;
    }

    return NULL;
}

static workspace_status_entry *
make_status_entry(const char *provider,
                  const char *status,
                  const char *summary,
                  const char *detail,
                  gboolean attention)
{
    workspace_status_entry *entry = g_new0(workspace_status_entry, 1);

    g_strlcpy(entry->provider, provider ? provider : "", sizeof(entry->provider));
    g_strlcpy(entry->kind, "status", sizeof(entry->kind));
    g_strlcpy(entry->status, status ? status : "", sizeof(entry->status));
    g_strlcpy(entry->summary, summary ? summary : "", sizeof(entry->summary));
    g_strlcpy(entry->detail, detail ? detail : "", sizeof(entry->detail));
    entry->updated_at_usec = 0;
    entry->attention = attention;

    return entry;
}

/* ---- Tests ---- */

static void
test_notification_preview_sanitization_and_suppression(void)
{
    GtkWidget *label;
    const char *text;
    const char *tooltip;

    require_display_or_skip();

    label = gtk_label_new(NULL);

    sidebar_ui_build_notification_preview_section(label, "hello", FALSE);
    g_assert_false(gtk_widget_get_visible(label));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(label)), ==, "");
    g_assert_null(gtk_widget_get_tooltip_text(label));

    sidebar_ui_build_notification_preview_section(
        label, "  build\nfailed\rnow  ", TRUE);
    g_assert_true(gtk_widget_get_visible(label));
    text = gtk_label_get_text(GTK_LABEL(label));
    tooltip = gtk_widget_get_tooltip_text(label);
    g_assert_cmpstr(text, ==, "build failed now");
    g_assert_cmpstr(tooltip, ==, "build failed now");

    sidebar_ui_build_notification_preview_section(label, " \n \r ", TRUE);
    g_assert_false(gtk_widget_get_visible(label));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(label)), ==, "");
    g_assert_null(gtk_widget_get_tooltip_text(label));
}

static void
test_branch_cwd_section_formats_and_suppresses(void)
{
    GtkWidget *label;
    const char *tooltip;

    require_display_or_skip();

    label = gtk_label_new(NULL);

    sidebar_ui_build_branch_cwd_section(label, "/tmp/demo", "main", FALSE);
    g_assert_false(gtk_widget_get_visible(label));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(label)), ==, "");
    g_assert_null(gtk_widget_get_tooltip_text(label));

    sidebar_ui_build_branch_cwd_section(label, "/tmp/demo", "main", TRUE);
    g_assert_true(gtk_widget_get_visible(label));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(label)), ==, "/tmp/demo [main]");
    tooltip = gtk_widget_get_tooltip_text(label);
    g_assert_cmpstr(tooltip, ==, "/tmp/demo");

    sidebar_ui_build_branch_cwd_section(label, "", "feature/phase8", TRUE);
    g_assert_true(gtk_widget_get_visible(label));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(label)), ==, "[feature/phase8]");
    g_assert_null(gtk_widget_get_tooltip_text(label));

    sidebar_ui_build_branch_cwd_section(label, "", "", TRUE);
    g_assert_false(gtk_widget_get_visible(label));
}

static void
test_status_section_renders_and_limits_entries(void)
{
    GtkWidget *box;
    g_autoptr(GPtrArray) entries = NULL;
    GtkWidget *first;
    GtkWidget *more;

    require_display_or_skip();

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    entries = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(entries, make_status_entry("codex", "running", "indexing",
                                               "indexing repository", TRUE));
    g_ptr_array_add(entries, make_status_entry("claude", "queued", "waiting",
                                               "", FALSE));

    sidebar_ui_build_workspace_status_section(box, entries, 1);

    g_assert_true(gtk_widget_get_visible(box));
    g_assert_cmpint(box_child_count(box), ==, 2);

    first = box_child_at(box, 0);
    g_assert_true(GTK_IS_LABEL(first));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(first)), ==,
                    "codex running: indexing");
    g_assert_cmpstr(gtk_widget_get_tooltip_text(first), ==,
                    "indexing repository");
    g_assert_true(gtk_widget_has_css_class(first, "sidebar-status"));
    g_assert_true(gtk_widget_has_css_class(first, "sidebar-status-entry"));
    g_assert_true(gtk_widget_has_css_class(first, "has-activity"));
    g_assert_cmpint(gtk_label_get_max_width_chars(GTK_LABEL(first)), ==, 30);
    g_assert_cmpint(gtk_label_get_ellipsize(GTK_LABEL(first)), ==,
                    PANGO_ELLIPSIZE_END);

    more = box_child_at(box, 1);
    g_assert_true(GTK_IS_LABEL(more));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(more)), ==, "+1 more");
}

static void
test_status_section_suppression_and_clear(void)
{
    GtkWidget *box;
    g_autoptr(GPtrArray) entries = NULL;

    require_display_or_skip();

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(box), gtk_label_new("stale"));
    g_assert_cmpint(box_child_count(box), ==, 1);

    sidebar_ui_build_workspace_status_section(box, NULL, 2);
    g_assert_false(gtk_widget_get_visible(box));
    g_assert_cmpint(box_child_count(box), ==, 0);

    entries = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(entries, make_status_entry("codex", "running", "indexing",
                                               "", FALSE));

    sidebar_ui_build_workspace_status_section(box, entries, 1);
    g_assert_true(gtk_widget_get_visible(box));
    g_assert_cmpint(box_child_count(box), ==, 1);

    sidebar_ui_build_workspace_status_section(box, entries, 0);
    g_assert_false(gtk_widget_get_visible(box));
    g_assert_cmpint(box_child_count(box), ==, 0);
}

static void
test_ports_section_sanitization_and_suppression(void)
{
    GtkWidget *label;

    require_display_or_skip();

    label = gtk_label_new(NULL);
    sidebar_ui_build_ports_section(label, "ports 3000 8080", FALSE);
    g_assert_false(gtk_widget_get_visible(label));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(label)), ==, "");

    sidebar_ui_build_ports_section(label, "  ports 3000 8080  ", TRUE);
    g_assert_true(gtk_widget_get_visible(label));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(label)), ==, "ports 3000 8080");
    g_assert_cmpstr(gtk_widget_get_tooltip_text(label), ==, "ports 3000 8080");

    sidebar_ui_build_ports_section(label, "  ", TRUE);
    g_assert_false(gtk_widget_get_visible(label));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(label)), ==, "");
}

static void
test_progress_section_formats_and_suppresses(void)
{
    GtkWidget *label;

    require_display_or_skip();

    label = gtk_label_new(NULL);
    sidebar_ui_build_progress_section(label, 0, 50, TRUE);
    g_assert_false(gtk_widget_get_visible(label));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(label)), ==, "");

    sidebar_ui_build_progress_section(label, 3, -1, TRUE);
    g_assert_true(gtk_widget_get_visible(label));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(label)), ==, "p~ [....]");
    g_assert_cmpstr(gtk_widget_get_tooltip_text(label), ==,
                    "Progress indeterminate");

    sidebar_ui_build_progress_section(label, 1, 63, TRUE);
    g_assert_true(gtk_widget_get_visible(label));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(label)), ==, "p [###-] 63%");
    g_assert_cmpstr(gtk_widget_get_tooltip_text(label), ==, "Progress");

    sidebar_ui_build_progress_section(label, 3, 63, TRUE);
    g_assert_true(gtk_widget_get_visible(label));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(label)), ==, "p~ [###-] 63%");
    g_assert_cmpstr(gtk_widget_get_tooltip_text(label), ==,
                    "Progress indeterminate");

    sidebar_ui_build_progress_section(label, 4, 50, TRUE);
    g_assert_true(gtk_widget_get_visible(label));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(label)), ==, "p|| [##--] 50%");

    sidebar_ui_build_progress_section(label, 2, -1, TRUE);
    g_assert_true(gtk_widget_get_visible(label));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(label)), ==, "p! [....]");
    g_assert_cmpstr(gtk_widget_get_tooltip_text(label), ==, "Progress error");

    sidebar_ui_build_progress_section(label, 4, -1, TRUE);
    g_assert_true(gtk_widget_get_visible(label));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(label)), ==, "p|| [....]");
    g_assert_cmpstr(gtk_widget_get_tooltip_text(label), ==, "Progress paused");
}

static void
test_structure_section_formats_and_suppresses(void)
{
    GtkWidget *label;

    require_display_or_skip();

    label = gtk_label_new(NULL);

    sidebar_ui_build_structure_indicator_section(label, FALSE, 1, 1, TRUE);
    g_assert_false(gtk_widget_get_visible(label));

    sidebar_ui_build_structure_indicator_section(label, FALSE, 2, 4, TRUE);
    g_assert_true(gtk_widget_get_visible(label));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(label)), ==, "P2 T4");

    sidebar_ui_build_structure_indicator_section(label, TRUE, 3, 0, TRUE);
    g_assert_true(gtk_widget_get_visible(label));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(label)), ==, "C3");

    sidebar_ui_build_structure_indicator_section(label, TRUE, 3, 0, FALSE);
    g_assert_false(gtk_widget_get_visible(label));
}

static void
test_workspace_card_sets_compact_truncation_properties(void)
{
    GtkWidget *header_box;
    GtkWidget *meta = NULL;
    GtkWidget *preview = NULL;
    GtkWidget *status_entries = NULL;
    GtkWidget *ports = NULL;
    GtkWidget *progress = NULL;
    GtkWidget *structure = NULL;
    GtkWidget *badge = NULL;
    GtkWidget *card;

    require_display_or_skip();

    header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    card = sidebar_ui_build_workspace_card(header_box,
                                           &meta,
                                           &preview,
                                           &status_entries,
                                           &ports,
                                           &progress,
                                           &structure,
                                           &badge);

    g_assert_true(GTK_IS_WIDGET(card));
    g_assert_true(GTK_IS_LABEL(meta));
    g_assert_true(GTK_IS_LABEL(preview));
    g_assert_true(GTK_IS_BOX(status_entries));
    g_assert_true(GTK_IS_LABEL(ports));
    g_assert_true(GTK_IS_LABEL(progress));
    g_assert_true(GTK_IS_LABEL(structure));
    g_assert_true(GTK_IS_LABEL(badge));

    g_assert_cmpint(gtk_label_get_max_width_chars(GTK_LABEL(meta)), ==, 26);
    g_assert_cmpint(gtk_label_get_ellipsize(GTK_LABEL(meta)), ==,
                    PANGO_ELLIPSIZE_END);
    g_assert_cmpint(gtk_label_get_max_width_chars(GTK_LABEL(preview)), ==, 30);
    g_assert_cmpint(gtk_label_get_ellipsize(GTK_LABEL(preview)), ==,
                    PANGO_ELLIPSIZE_END);
    g_assert_cmpint(gtk_label_get_max_width_chars(GTK_LABEL(ports)), ==, 16);
    g_assert_cmpint(gtk_label_get_max_width_chars(GTK_LABEL(progress)), ==, 16);
    g_assert_cmpint(gtk_label_get_max_width_chars(GTK_LABEL(structure)), ==, 10);
    g_assert_false(gtk_widget_get_visible(meta));
    g_assert_false(gtk_widget_get_visible(preview));
    g_assert_false(gtk_widget_get_visible(status_entries));
    g_assert_false(gtk_widget_get_visible(ports));
    g_assert_false(gtk_widget_get_visible(progress));
    g_assert_false(gtk_widget_get_visible(structure));
    g_assert_false(gtk_widget_get_visible(badge));
}

static void
test_row_activation_prefers_rename_entry_over_workspace_switch(void)
{
    GtkWidget *row;
    GtkWidget *card;
    GtkWidget *header_box;
    GtkWidget *rename_entry;
    GtkWidget *meta = NULL, *status = NULL, *status_entries = NULL;
    GtkWidget *ports = NULL, *progress = NULL, *structure = NULL, *badge = NULL;

    require_display_or_skip();
    reset_stub_counters();

    ui.terminal_stack = gtk_stack_new();
    sidebar_ui_build();

    row = gtk_list_box_row_new();
    header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    card = sidebar_ui_build_workspace_card(header_box,
                                           &meta, &status, &status_entries,
                                           &ports, &progress, &structure, &badge);
    rename_entry = gtk_entry_new();
    g_object_set_data(G_OBJECT(header_box), "rename-entry", rename_entry);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), card);
    gtk_list_box_append(GTK_LIST_BOX(ui.workspace_list), row);

    g_signal_emit_by_name(ui.workspace_list, "row-activated",
                          GTK_LIST_BOX_ROW(row));

    g_assert_cmpint(workspace_switch_call_count, ==, 0);
    g_assert_cmpint(session_queue_save_call_count, ==, 0);
}

static void
test_row_activation_prefers_rename_in_progress_over_workspace_switch(void)
{
    GtkWidget *row;
    GtkWidget *card;
    GtkWidget *header_box;
    GtkWidget *meta = NULL, *status = NULL, *status_entries = NULL;
    GtkWidget *ports = NULL, *progress = NULL, *structure = NULL, *badge = NULL;

    require_display_or_skip();
    reset_stub_counters();

    ui.terminal_stack = gtk_stack_new();
    sidebar_ui_build();

    row = gtk_list_box_row_new();
    header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    g_object_set_data(G_OBJECT(header_box), "rename-in-progress",
                      GINT_TO_POINTER(1));
    card = sidebar_ui_build_workspace_card(header_box,
                                           &meta, &status, &status_entries,
                                           &ports, &progress, &structure, &badge);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), card);
    gtk_list_box_append(GTK_LIST_BOX(ui.workspace_list), row);

    g_signal_emit_by_name(ui.workspace_list, "row-activated",
                          GTK_LIST_BOX_ROW(row));

    g_assert_cmpint(workspace_switch_call_count, ==, 0);
    g_assert_cmpint(session_queue_save_call_count, ==, 0);
}

static void
test_row_activation_switches_workspace_when_not_renaming(void)
{
    GtkWidget *row0;
    GtkWidget *row1;
    GtkWidget *card0;
    GtkWidget *card1;
    GtkWidget *header0;
    GtkWidget *header1;
    GtkWidget *meta = NULL, *status = NULL, *status_entries = NULL;
    GtkWidget *ports = NULL, *progress = NULL, *structure = NULL, *badge = NULL;

    require_display_or_skip();
    reset_stub_counters();

    ui.terminal_stack = gtk_stack_new();
    sidebar_ui_build();

    row0 = gtk_list_box_row_new();
    header0 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    card0 = sidebar_ui_build_workspace_card(header0,
                                            &meta, &status, &status_entries,
                                            &ports, &progress, &structure, &badge);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row0), card0);
    gtk_list_box_append(GTK_LIST_BOX(ui.workspace_list), row0);

    row1 = gtk_list_box_row_new();
    header1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    card1 = sidebar_ui_build_workspace_card(header1,
                                            &meta, &status, &status_entries,
                                            &ports, &progress, &structure, &badge);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row1), card1);
    gtk_list_box_append(GTK_LIST_BOX(ui.workspace_list), row1);

    g_signal_emit_by_name(ui.workspace_list, "row-activated",
                          GTK_LIST_BOX_ROW(row1));

    g_assert_true(wait_for_workspace_switch_count(1));
    g_assert_cmpint(workspace_switch_call_count, ==, 1);
    g_assert_cmpint(workspace_switch_last_index, ==, 1);
    g_assert_cmpint(session_queue_save_call_count, ==, 1);
}

static void
test_move_to_window_menu_routes_to_workspace_target_modal(void)
{
    Workspace ws = {0};

    require_display_or_skip();
    reset_stub_counters();

    ui.overlay = gtk_overlay_new();
    ui.terminal_stack = gtk_stack_new();
    ui.workspace_list = gtk_list_box_new();

    sidebar_ui_show_move_to_window_menu(&ws);

    g_assert_cmpint(move_to_window_modal_call_count, ==, 1);
    g_assert_true(move_to_window_last_overlay == GTK_OVERLAY(ui.overlay));
    g_assert_true(move_to_window_last_terminal_stack == ui.terminal_stack);
    g_assert_true(move_to_window_last_workspace_list == ui.workspace_list);
    g_assert_cmpint(move_to_window_last_workspace_index, ==, 0);
}

static void
test_move_to_window_menu_ignores_invalid_state(void)
{
    Workspace ws = {0};

    require_display_or_skip();
    reset_stub_counters();

    ui.overlay = NULL;
    ui.terminal_stack = gtk_stack_new();
    ui.workspace_list = gtk_list_box_new();
    sidebar_ui_show_move_to_window_menu(&ws);
    g_assert_cmpint(move_to_window_modal_call_count, ==, 0);

    ui.overlay = gtk_overlay_new();
    sidebar_ui_show_move_to_window_menu(NULL);
    g_assert_cmpint(move_to_window_modal_call_count, ==, 0);
}

static void
test_card_preserves_header_box_and_close_button(void)
{
    GtkWidget *header_box;
    GtkWidget *close_btn;
    GtkWidget *meta = NULL, *status = NULL, *status_entries = NULL;
    GtkWidget *ports = NULL, *progress = NULL, *structure = NULL, *badge = NULL;
    GtkWidget *card;
    GtkWidget *retrieved_header;

    require_display_or_skip();

    header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    close_btn = gtk_button_new_with_label("x");
    gtk_box_append(GTK_BOX(header_box), close_btn);

    card = sidebar_ui_build_workspace_card(header_box,
                                           &meta, &status, &status_entries,
                                           &ports, &progress, &structure, &badge);

    retrieved_header = g_object_get_data(G_OBJECT(card), "header-box");
    g_assert_true(retrieved_header == header_box);
    g_assert_true(gtk_widget_get_first_child(header_box) == close_btn);
    g_assert_true(GTK_IS_BUTTON(close_btn));
}

static void
test_card_workspace_data_survives_wrapping(void)
{
    GtkWidget *header_box;
    GtkWidget *meta = NULL, *status = NULL, *status_entries = NULL;
    GtkWidget *ports = NULL, *progress = NULL, *structure = NULL, *badge = NULL;
    GtkWidget *card;
    Workspace ws = {0};

    require_display_or_skip();

    header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    card = sidebar_ui_build_workspace_card(header_box,
                                           &meta, &status, &status_entries,
                                           &ports, &progress, &structure, &badge);

    g_object_set_data(G_OBJECT(card), "workspace", &ws);
    g_assert_true(g_object_get_data(G_OBJECT(card), "workspace") == &ws);
}

static void
test_rename_entry_fallback_on_card_child(void)
{
    GtkWidget *row;
    GtkWidget *card;
    GtkWidget *header_box;
    GtkWidget *rename_entry;

    require_display_or_skip();
    reset_stub_counters();

    ui.terminal_stack = gtk_stack_new();
    sidebar_ui_build();

    row = gtk_list_box_row_new();
    card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    rename_entry = gtk_entry_new();
    g_object_set_data(G_OBJECT(card), "rename-entry", rename_entry);
    g_object_set_data(G_OBJECT(card), "header-box", header_box);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), card);
    gtk_list_box_append(GTK_LIST_BOX(ui.workspace_list), row);

    g_signal_emit_by_name(ui.workspace_list, "row-activated",
                          GTK_LIST_BOX_ROW(row));

    g_assert_cmpint(workspace_switch_call_count, ==, 0);
    g_assert_cmpint(session_queue_save_call_count, ==, 0);
}

static void
test_search_filter_exercises_workspace_fields(void)
{
    GtkWidget *row;
    GtkWidget *card;
    Workspace ws = {0};

    require_display_or_skip();

    ui.terminal_stack = gtk_stack_new();
    sidebar_ui_build();

    g_strlcpy(ws.name, "devbox", sizeof(ws.name));
    g_strlcpy(ws.cwd, "/home/user/projects/app", sizeof(ws.cwd));
    g_strlcpy(ws.git_branch, "feature/sidebar", sizeof(ws.git_branch));

    row = gtk_list_box_row_new();
    card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    g_object_set_data(G_OBJECT(card), "workspace", &ws);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), card);
    gtk_list_box_append(GTK_LIST_BOX(ui.workspace_list), row);

    gtk_editable_set_text(GTK_EDITABLE(ui.workspace_search), "devbox");
    gtk_list_box_invalidate_filter(GTK_LIST_BOX(ui.workspace_list));

    gtk_editable_set_text(GTK_EDITABLE(ui.workspace_search), "projects");
    gtk_list_box_invalidate_filter(GTK_LIST_BOX(ui.workspace_list));

    gtk_editable_set_text(GTK_EDITABLE(ui.workspace_search), "sidebar");
    gtk_list_box_invalidate_filter(GTK_LIST_BOX(ui.workspace_list));

    gtk_editable_set_text(GTK_EDITABLE(ui.workspace_search), "nonexistent");
    gtk_list_box_invalidate_filter(GTK_LIST_BOX(ui.workspace_list));

    gtk_editable_set_text(GTK_EDITABLE(ui.workspace_search), "");
    gtk_list_box_invalidate_filter(GTK_LIST_BOX(ui.workspace_list));
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    have_display = gtk_init_check();

    g_test_add_func("/sidebar-ui/notification-preview/sanitize-suppress",
                    test_notification_preview_sanitization_and_suppression);
    g_test_add_func("/sidebar-ui/branch-cwd/format-suppress",
                    test_branch_cwd_section_formats_and_suppresses);
    g_test_add_func("/sidebar-ui/status/renders-limit",
                    test_status_section_renders_and_limits_entries);
    g_test_add_func("/sidebar-ui/status/suppress-clear",
                    test_status_section_suppression_and_clear);
    g_test_add_func("/sidebar-ui/ports/sanitize-suppress",
                    test_ports_section_sanitization_and_suppression);
    g_test_add_func("/sidebar-ui/progress/format-suppress",
                    test_progress_section_formats_and_suppresses);
    g_test_add_func("/sidebar-ui/structure/format-suppress",
                    test_structure_section_formats_and_suppresses);
    g_test_add_func("/sidebar-ui/card/compact-truncation",
                    test_workspace_card_sets_compact_truncation_properties);
    g_test_add_func("/sidebar-ui/row-activation/rename-short-circuit",
                    test_row_activation_prefers_rename_entry_over_workspace_switch);
    g_test_add_func("/sidebar-ui/row-activation/rename-in-progress-short-circuit",
                    test_row_activation_prefers_rename_in_progress_over_workspace_switch);
    g_test_add_func("/sidebar-ui/row-activation/switch-and-save",
                    test_row_activation_switches_workspace_when_not_renaming);
    g_test_add_func("/sidebar-ui/move-to-window/routes-modal",
                    test_move_to_window_menu_routes_to_workspace_target_modal);
    g_test_add_func("/sidebar-ui/move-to-window/invalid-state",
                    test_move_to_window_menu_ignores_invalid_state);
    g_test_add_func("/sidebar-ui/card/header-box-close-button",
                    test_card_preserves_header_box_and_close_button);
    g_test_add_func("/sidebar-ui/card/workspace-data-survives-wrapping",
                    test_card_workspace_data_survives_wrapping);
    g_test_add_func("/sidebar-ui/row-activation/rename-fallback-on-child",
                    test_rename_entry_fallback_on_card_child);
    g_test_add_func("/sidebar-ui/filter/workspace-struct-fields",
                    test_search_filter_exercises_workspace_fields);

    return g_test_run();
}
