/*
 * theme_selector.c - Per-scope ghostty color-theme picker (see header).
 */
#include "theme_selector.h"

#include <adwaita.h>
#include <string.h>

#include "app_settings.h"
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

char **
theme_selector_list_themes(void)
{
    static char **cached = NULL;

    if (cached)
        return cached;

    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, NULL);
    GPtrArray *names = g_ptr_array_new();

    /* Scan the same theme directories app_settings resolves paths against. */
    char **dirs = app_settings_ghostty_theme_dirs();
    for (int i = 0; dirs[i]; i++)
        theme_names_add_dir(seen, names, dirs[i]);
    g_strfreev(dirs);

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
            if (!line[0] || g_hash_table_contains(seen, line))
                continue;
            /* Only offer a CLI-reported theme we can actually resolve: the
             * external `ghostty` binary may see themes this build cannot. */
            char *probe = app_settings_resolve_ghostty_theme_path(line);
            if (!probe)
                continue;
            g_free(probe);
            g_hash_table_add(seen, g_strdup(line));
            g_ptr_array_add(names, g_strdup(line));
        }
        g_strfreev(lines);
    }
    g_free(out);
    g_hash_table_destroy(seen);

    /* Deliberately no hardcoded fallback list: in a packaged install with no
     * ghostty theme files, every such name would fail to resolve and applying
     * it would silently do nothing. An empty list plus "Inherit (default)" is
     * the honest state. */

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
    GtkDropDown *scope_dropdown;   /* NULL when the dialog has a fixed scope */
    ThemeScope   fixed_scope;      /* used when scope_dropdown == NULL */
    GtkDropDown *theme_dropdown;
    GWeakRef     term_ref;         /* tab target   (may be unset) */
    GWeakRef     pane_ref;         /* pane target  (may be unset) */
    guint64      ws_serial;        /* workspace target (0 = unset) */
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

/* Current override string for a scope (newly allocated, or NULL to inherit). */
static char *
theme_sel_current_for_scope(ThemeSelCtx *ctx, ThemeScope scope)
{
    char *out = NULL;
    switch (scope) {
    case THEME_SCOPE_TAB: {
        GhosttyTerminal *t = g_weak_ref_get(&ctx->term_ref);
        if (t) {
            out = g_strdup(ghostty_terminal_get_theme_override(t));
            g_object_unref(t);
        }
        break;
    }
    case THEME_SCOPE_PANE: {
        GtkNotebook *p = g_weak_ref_get(&ctx->pane_ref);
        if (p) {
            out = g_strdup(g_object_get_data(G_OBJECT(p), "pane-theme"));
            g_object_unref(p);
        }
        break;
    }
    case THEME_SCOPE_WORKSPACE: {
        Workspace *ws = theme_sel_find_workspace(ctx->ws_serial);
        if (ws)
            out = g_strdup(ws->theme_name);
        break;
    }
    }
    return out;
}

/* Apply the chosen theme (NULL/"" == inherit from the next scope up) to the
 * given scope's target, re-theme affected live surfaces, and persist. */
static void
theme_sel_apply(ThemeSelCtx *ctx, ThemeScope scope, const char *name)
{
    gboolean changed = FALSE;

    switch (scope) {
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

/* Select `name` in the theme dropdown (index 0 is "Inherit (default)").
 * NULL/"" selects Inherit; a value not already present is appended. */
static void
theme_sel_select_theme(GtkDropDown *dd, const char *name)
{
    if (!name || !name[0]) {
        gtk_drop_down_set_selected(dd, 0);
        return;
    }
    GtkStringList *model = GTK_STRING_LIST(gtk_drop_down_get_model(dd));
    guint n = g_list_model_get_n_items(G_LIST_MODEL(model));
    for (guint i = 1; i < n; i++) {
        if (g_strcmp0(gtk_string_list_get_string(model, i), name) == 0) {
            gtk_drop_down_set_selected(dd, i);
            return;
        }
    }
    gtk_string_list_append(model, name);
    gtk_drop_down_set_selected(dd, n);
}

/* When the scope changes, reflect that scope's current override in the theme
 * dropdown so the dialog always shows what is actually applied there. */
static void
on_theme_scope_changed(GObject *obj, GParamSpec *pspec, gpointer user_data)
{
    (void)obj;
    (void)pspec;
    ThemeSelCtx *ctx = user_data;
    ThemeScope scope = (ThemeScope)gtk_drop_down_get_selected(ctx->scope_dropdown);
    char *cur = theme_sel_current_for_scope(ctx, scope);
    theme_sel_select_theme(ctx->theme_dropdown, cur);
    g_free(cur);
}

static void
on_theme_dialog_response(AdwMessageDialog *dialog, const char *response,
                         gpointer user_data)
{
    (void)dialog;
    ThemeSelCtx *ctx = user_data;

    if (g_strcmp0(response, "apply") != 0)
        return;

    ThemeScope scope = ctx->scope_dropdown
        ? (ThemeScope)gtk_drop_down_get_selected(ctx->scope_dropdown)
        : ctx->fixed_scope;

    /* Read the selected string straight from the model. Index 0 is
     * "Inherit (default)" -> NULL (clear the override). Reading the item
     * directly (rather than indexing a parallel array) keeps this correct
     * even for an appended not-in-list current value. */
    guint sel = gtk_drop_down_get_selected(ctx->theme_dropdown);
    if (sel == GTK_INVALID_LIST_POSITION || sel == 0) {
        theme_sel_apply(ctx, scope, NULL);
        return;
    }

    GObject *item = g_list_model_get_item(
        gtk_drop_down_get_model(ctx->theme_dropdown), sel);
    if (!item)
        return;
    char *chosen = g_strdup(
        gtk_string_object_get_string(GTK_STRING_OBJECT(item)));
    g_object_unref(item);

    theme_sel_apply(ctx, scope, (chosen && chosen[0]) ? chosen : NULL);
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
theme_selector_open(GtkWidget *anchor, gboolean with_scope_selector,
                    ThemeScope initial_scope, GhosttyTerminal *term,
                    GtkNotebook *pane, guint64 ws_serial,
                    const char *heading, const char *current)
{
    char **names = theme_selector_list_themes();

    ThemeSelCtx *ctx = g_new0(ThemeSelCtx, 1);
    ctx->fixed_scope = initial_scope;
    ctx->ws_serial = ws_serial;
    g_weak_ref_init(&ctx->term_ref, term);
    g_weak_ref_init(&ctx->pane_ref, pane);
    ctx->theme_dropdown = theme_sel_build_dropdown(names, current);

    GtkWidget *extra;
    if (with_scope_selector) {
        GtkStringList *scopes = gtk_string_list_new(NULL);
        gtk_string_list_append(scopes, "This tab");
        gtk_string_list_append(scopes, "This pane");
        gtk_string_list_append(scopes, "This workspace");
        ctx->scope_dropdown =
            GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(scopes), NULL));
        gtk_drop_down_set_selected(ctx->scope_dropdown, (guint)initial_scope);
        g_signal_connect(ctx->scope_dropdown, "notify::selected",
                         G_CALLBACK(on_theme_scope_changed), ctx);

        GtkWidget *scope_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *scope_label = gtk_label_new("Apply to");
        gtk_widget_set_halign(GTK_WIDGET(ctx->scope_dropdown), GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(scope_row), scope_label);
        gtk_box_append(GTK_BOX(scope_row), GTK_WIDGET(ctx->scope_dropdown));

        extra = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_box_append(GTK_BOX(extra), scope_row);
        gtk_box_append(GTK_BOX(extra), GTK_WIDGET(ctx->theme_dropdown));
    } else {
        extra = GTK_WIDGET(ctx->theme_dropdown);
    }

    GtkWindow *parent = NULL;
    if (anchor) {
        GtkRoot *root = gtk_widget_get_root(anchor);
        if (root && GTK_IS_WINDOW(root))
            parent = GTK_WINDOW(root);
    }
    if (!parent && g_main_window)
        parent = g_main_window;

    AdwMessageDialog *dialog =
        ADW_MESSAGE_DIALOG(adw_message_dialog_new(parent, heading, NULL));
    adw_message_dialog_set_body(
        dialog, "Pick a ghostty color theme, or inherit the default.");
    adw_message_dialog_set_extra_child(dialog, extra);
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
theme_selector_popup_for_terminal(GtkWidget *anchor, GhosttyTerminal *term)
{
    if (!term || !GHOSTTY_IS_TERMINAL(term))
        return;

    /* Resolve pane + workspace so the dialog's scope selector can target
     * any of the three levels for this terminal. */
    GtkWidget *dummy = ghostty_terminal_get_dummy_target(term);
    GtkWidget *anc = dummy ? gtk_widget_get_ancestor(dummy, GTK_TYPE_NOTEBOOK)
                           : NULL;
    GtkNotebook *pane = GTK_IS_NOTEBOOK(anc) ? GTK_NOTEBOOK(anc) : NULL;
    Workspace *ws = pane ? g_object_get_data(G_OBJECT(pane), "workspace-ptr")
                         : NULL;

    theme_selector_open(anchor, TRUE, THEME_SCOPE_TAB, term, pane,
                        ws ? ws->serial : 0, "Terminal theme",
                        ghostty_terminal_get_theme_override(term));
}

void
theme_selector_popup_tab(GtkWidget *anchor, GhosttyTerminal *term)
{
    if (!term || !GHOSTTY_IS_TERMINAL(term))
        return;
    theme_selector_open(anchor, FALSE, THEME_SCOPE_TAB, term, NULL, 0,
                        "Theme for this tab",
                        ghostty_terminal_get_theme_override(term));
}

void
theme_selector_popup_pane(GtkWidget *anchor, GtkNotebook *pane)
{
    if (!pane || !GTK_IS_NOTEBOOK(pane))
        return;
    theme_selector_open(anchor, FALSE, THEME_SCOPE_PANE, NULL, pane, 0,
                        "Theme for this pane",
                        g_object_get_data(G_OBJECT(pane), "pane-theme"));
}

void
theme_selector_popup_workspace(GtkWidget *anchor, Workspace *ws)
{
    if (!ws)
        return;
    theme_selector_open(anchor, FALSE, THEME_SCOPE_WORKSPACE, NULL, NULL,
                        ws->serial, "Theme for this workspace", ws->theme_name);
}
