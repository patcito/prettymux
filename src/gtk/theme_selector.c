/*
 * theme_selector.c - Per-scope ghostty color-theme picker (see header).
 */
#include "theme_selector.h"

#include <adwaita.h>
#include <string.h>

#include "app_state.h"
#include "app_ui.h"
#include "session.h"

/* ── Available ghostty theme names ─────────────────────────────────
 *
 * ghostty theme files live one-per-name in a set of theme directories (the
 * same ones libghostty reads). We enumerate those directories directly —
 * the filename IS the theme name — which is robust even when the `ghostty`
 * CLI is absent (packaged installs) or refuses to list (`+list-themes`
 * returns "No themes found" on some setups). The CLI output, if any, is
 * merged in as a supplementary source. Results are deduped, sorted, and
 * cached for the process lifetime. A small hardcoded fallback covers the
 * case where nothing is found at all.
 */
static void
theme_names_add_dir(GHashTable *seen, GPtrArray *out, const char *dir)
{
    if (!dir || !dir[0])
        return;
    GDir *d = g_dir_open(dir, 0, NULL);
    if (!d)
        return;
    const char *name;
    while ((name = g_dir_read_name(d))) {
        char *full = g_build_filename(dir, name, NULL);
        gboolean is_regular = g_file_test(full, G_FILE_TEST_IS_REGULAR);
        g_free(full);
        if (!is_regular)
            continue;
        if (!g_hash_table_contains(seen, name)) {
            g_hash_table_add(seen, g_strdup(name));
            g_ptr_array_add(out, g_strdup(name));
        }
    }
    g_dir_close(d);
}

static int
theme_names_cmp(gconstpointer a, gconstpointer b)
{
    return g_ascii_strcasecmp(*(const char *const *)a, *(const char *const *)b);
}

static char **
theme_selector_theme_names(void)
{
    static char **cached = NULL;

    if (cached)
        return cached;

    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, NULL);
    GPtrArray *names = g_ptr_array_new();

    /* User themes, then $GHOSTTY_RESOURCES_DIR, then every system data dir. */
    char *user_dir = g_build_filename(g_get_user_config_dir(), "ghostty",
                                      "themes", NULL);
    theme_names_add_dir(seen, names, user_dir);
    g_free(user_dir);

    const char *res = g_getenv("GHOSTTY_RESOURCES_DIR");
    if (res && res[0]) {
        char *rd = g_build_filename(res, "themes", NULL);
        theme_names_add_dir(seen, names, rd);
        g_free(rd);
    }

    const char *const *sys = g_get_system_data_dirs();
    for (int i = 0; sys && sys[i]; i++) {
        char *sd = g_build_filename(sys[i], "ghostty", "themes", NULL);
        theme_names_add_dir(seen, names, sd);
        g_free(sd);
    }

    /* Supplement with the CLI list if available (strip only the trailing
     * " (source)" annotation, so names that themselves contain parentheses
     * — e.g. "Black Metal (Bathory)" — survive intact). */
    char *out = NULL;
    if (g_spawn_command_line_sync("ghostty +list-themes", &out, NULL, NULL, NULL) &&
        out && out[0]) {
        char **lines = g_strsplit(out, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            char *line = g_strstrip(lines[i]);
            if (!line[0])
                continue;
            size_t len = strlen(line);
            if (line[len - 1] == ')') {
                char *paren = g_strrstr(line, " (");
                if (paren)
                    *paren = '\0';
            }
            g_strstrip(line);
            if (line[0] && !g_hash_table_contains(seen, line)) {
                g_hash_table_add(seen, g_strdup(line));
                g_ptr_array_add(names, g_strdup(line));
            }
        }
        g_strfreev(lines);
    }
    g_free(out);
    g_hash_table_destroy(seen);

    if (names->len == 0) {
        static const char *fallback[] = {
            "Catppuccin Mocha", "Catppuccin Frappe", "Catppuccin Latte",
            "Adwaita Dark", "Adwaita", "Light Owl", NULL,
        };
        for (int i = 0; fallback[i]; i++)
            g_ptr_array_add(names, g_strdup(fallback[i]));
    }

    g_ptr_array_sort(names, theme_names_cmp);
    g_ptr_array_add(names, NULL);
    cached = (char **)g_ptr_array_free(names, FALSE);
    return cached;
}

/* ── Dialog context ────────────────────────────────────────────────── */

typedef enum {
    THEME_SCOPE_TAB,
    THEME_SCOPE_PANE,
    THEME_SCOPE_WORKSPACE,
} ThemeScope;

typedef struct {
    ThemeScope   scope;
    GtkDropDown *dropdown;
    GWeakRef     term_ref;   /* THEME_SCOPE_TAB */
    GWeakRef     pane_ref;   /* THEME_SCOPE_PANE */
    guint64      ws_serial;  /* THEME_SCOPE_WORKSPACE */
} ThemeSelCtx;

static void
theme_sel_ctx_free(gpointer data)
{
    ThemeSelCtx *ctx = data;
    g_weak_ref_clear(&ctx->term_ref);
    g_weak_ref_clear(&ctx->pane_ref);
    g_free(ctx);
}

static Workspace *
theme_sel_find_workspace(guint64 serial)
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

/* Apply the chosen theme (NULL/"" == inherit from the next scope up) to the
 * dialog's target, re-theme affected live surfaces, and persist. */
static void
theme_sel_apply(ThemeSelCtx *ctx, const char *name)
{
    gboolean changed = FALSE;

    switch (ctx->scope) {
    case THEME_SCOPE_TAB: {
        GhosttyTerminal *term = g_weak_ref_get(&ctx->term_ref);
        if (term) {
            ghostty_terminal_set_theme_override(term, name);
            g_object_unref(term);
            changed = TRUE;
        }
        break;
    }
    case THEME_SCOPE_PANE: {
        GtkNotebook *pane = g_weak_ref_get(&ctx->pane_ref);
        if (pane) {
            g_object_set_data_full(G_OBJECT(pane), "pane-theme",
                                   (name && name[0]) ? g_strdup(name) : NULL,
                                   g_free);
            g_object_unref(pane);
            changed = TRUE;
        }
        break;
    }
    case THEME_SCOPE_WORKSPACE: {
        Workspace *ws = theme_sel_find_workspace(ctx->ws_serial);
        if (ws) {
            g_free(ws->theme_name);
            ws->theme_name = (name && name[0]) ? g_strdup(name) : NULL;
            changed = TRUE;
        }
        break;
    }
    }

    if (changed) {
        app_apply_scoped_terminal_themes();
        session_queue_save();
    }
}

static void
on_theme_dialog_response(AdwMessageDialog *dialog, const char *response,
                         gpointer user_data)
{
    (void)dialog;
    ThemeSelCtx *ctx = user_data;

    if (g_strcmp0(response, "apply") != 0)
        return;

    /* Read the selected string straight from the model. Index 0 is
     * "Inherit (default)" -> NULL (clear the override). Reading the item
     * directly (rather than indexing a parallel array) keeps this correct
     * even for an appended not-in-list current value. */
    guint sel = gtk_drop_down_get_selected(ctx->dropdown);
    if (sel == GTK_INVALID_LIST_POSITION || sel == 0) {
        if (sel == 0)
            theme_sel_apply(ctx, NULL);
        return;
    }

    GObject *item = g_list_model_get_item(
        gtk_drop_down_get_model(ctx->dropdown), sel);
    if (!item)
        return;
    char *chosen = g_strdup(
        gtk_string_object_get_string(GTK_STRING_OBJECT(item)));
    g_object_unref(item);

    theme_sel_apply(ctx, (chosen && chosen[0]) ? chosen : NULL);
    g_free(chosen);
}

/* Build the [Inherit, theme...] dropdown, preselecting `current`. */
static GtkDropDown *
theme_sel_build_dropdown(char **names, const char *current)
{
    GtkStringList *model = gtk_string_list_new(NULL);
    gtk_string_list_append(model, "Inherit (default)");
    guint selected = 0;
    gboolean found = FALSE;
    for (int i = 0; names[i]; i++) {
        gtk_string_list_append(model, names[i]);
        if (current && current[0] && g_strcmp0(current, names[i]) == 0) {
            selected = (guint)(i + 1);
            found = TRUE;
        }
    }
    /* Preserve an override whose theme is not in the enumerated list, so
     * opening the dialog and pressing Apply doesn't silently erase it. */
    if (current && current[0] && !found) {
        gtk_string_list_append(model, current);
        selected = g_list_model_get_n_items(G_LIST_MODEL(model)) - 1;
    }

    GtkExpression *expr = gtk_property_expression_new(
        GTK_TYPE_STRING_OBJECT, NULL, "string");
    GtkWidget *dd = gtk_drop_down_new(G_LIST_MODEL(model), expr);
    gtk_drop_down_set_enable_search(GTK_DROP_DOWN(dd), TRUE);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(dd), selected);
    return GTK_DROP_DOWN(dd);
}

static void
theme_selector_open(GtkWidget *anchor, ThemeScope scope,
                    GhosttyTerminal *term, GtkNotebook *pane, guint64 ws_serial,
                    const char *scope_label, const char *current)
{
    char **names = theme_selector_theme_names();

    ThemeSelCtx *ctx = g_new0(ThemeSelCtx, 1);
    ctx->scope = scope;
    ctx->ws_serial = ws_serial;
    g_weak_ref_init(&ctx->term_ref, scope == THEME_SCOPE_TAB ? term : NULL);
    g_weak_ref_init(&ctx->pane_ref, scope == THEME_SCOPE_PANE ? pane : NULL);
    ctx->dropdown = theme_sel_build_dropdown(names, current);

    GtkWindow *parent = NULL;
    if (anchor) {
        GtkRoot *root = gtk_widget_get_root(anchor);
        if (root && GTK_IS_WINDOW(root))
            parent = GTK_WINDOW(root);
    }
    if (!parent && g_main_window)
        parent = g_main_window;

    char *heading = g_strdup_printf("Theme for %s", scope_label);
    AdwMessageDialog *dialog =
        ADW_MESSAGE_DIALOG(adw_message_dialog_new(parent, heading, NULL));
    g_free(heading);
    adw_message_dialog_set_body(
        dialog, "Pick a ghostty color theme, or inherit the default.");
    adw_message_dialog_set_extra_child(dialog, GTK_WIDGET(ctx->dropdown));
    adw_message_dialog_add_response(dialog, "cancel", "Cancel");
    adw_message_dialog_add_response(dialog, "apply", "Apply");
    adw_message_dialog_set_response_appearance(dialog, "apply",
                                               ADW_RESPONSE_SUGGESTED);
    adw_message_dialog_set_default_response(dialog, "apply");
    adw_message_dialog_set_close_response(dialog, "cancel");

    g_object_set_data_full(G_OBJECT(dialog), "theme-sel-ctx", ctx,
                           theme_sel_ctx_free);
    g_signal_connect(dialog, "response",
                     G_CALLBACK(on_theme_dialog_response), ctx);

    gtk_window_present(GTK_WINDOW(dialog));
}

/* ── Public entry points ───────────────────────────────────────────── */

void
theme_selector_popup_tab(GtkWidget *anchor, GhosttyTerminal *term)
{
    if (!term)
        return;
    theme_selector_open(anchor, THEME_SCOPE_TAB, term, NULL, 0,
                        "this tab", ghostty_terminal_get_theme_override(term));
}

void
theme_selector_popup_pane(GtkWidget *anchor, GtkNotebook *pane)
{
    if (!pane)
        return;
    theme_selector_open(anchor, THEME_SCOPE_PANE, NULL, pane, 0,
                        "this pane",
                        g_object_get_data(G_OBJECT(pane), "pane-theme"));
}

void
theme_selector_popup_workspace(GtkWidget *anchor, Workspace *ws)
{
    if (!ws)
        return;
    theme_selector_open(anchor, THEME_SCOPE_WORKSPACE, NULL, NULL, ws->serial,
                        "this workspace", ws->theme_name);
}
