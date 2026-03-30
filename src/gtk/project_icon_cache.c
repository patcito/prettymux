#include "project_icon_cache.h"

#include <gio/gio.h>
#include <string.h>

#define ICON_SEARCH_TIMEOUT_USEC (3 * G_USEC_PER_SEC)
#define ICON_SEARCH_SHALLOW_DEPTH 3
#define ICON_SEARCH_MAX_DEPTH 6

typedef struct {
    ProjectIconResolvedFunc callback;
    gpointer user_data;
    GDestroyNotify destroy;
} PendingListener;

typedef struct {
    char *root;
    char *icon_path;
} SearchResult;

typedef struct {
    char *root;
    char *icon_path;
    ProjectIconResolvedFunc callback;
    gpointer user_data;
    GDestroyNotify destroy;
} IdleDispatch;

typedef struct {
    char *path;
    int depth;
    gboolean preferred_context;
} DirQueueItem;

static GHashTable *icon_cache = NULL;   /* root -> icon path or "" */
static GHashTable *pending = NULL;      /* root -> GPtrArray<PendingListener*> */

static const char *project_markers[] = {
    ".git", ".hg", ".svn",
    "package.json", "package-lock.json",
    "pnpm-lock.yaml", "pnpm-workspace.yaml", "yarn.lock",
    "bun.lock", "bun.lockb",
    "pyproject.toml", "Cargo.toml", "go.mod",
    "composer.json", "mix.exs",
    "deno.json", "deno.jsonc",
    NULL
};

static const char *candidate_names[] = {
    "favicon.ico",
    "favicon.png",
    "favicon.svg",
    "favicon.webp",
    "apple-touch-icon.png",
    "apple-touch-icon-precomposed.png",
    "icon.svg",
    "icon.png",
    "icon.ico",
    "logo.svg",
    "logo.png",
    "logo.ico",
    NULL
};

static const char *preferred_dirs[] = {
    "public", "assets", "asset", "static",
    "img", "image", "images",
    "icon", "icons", "logo", "logos",
    NULL
};

static const char *pruned_dirs[] = {
    ".git", ".hg", ".svn",
    "node_modules", "vendor", "dist", "build",
    ".next", ".nuxt", ".cache", ".turbo",
    ".yarn", ".pnpm-store", "coverage", "target",
    "tmp", "temp", ".venv", "venv", "__pycache__",
    NULL
};

static void
pending_listener_free(gpointer data)
{
    PendingListener *listener = data;

    if (listener->destroy)
        listener->destroy(listener->user_data);
    g_free(listener);
}

static void
dir_queue_item_free(DirQueueItem *item)
{
    if (!item)
        return;
    g_free(item->path);
    g_free(item);
}

static void
search_result_free(SearchResult *result)
{
    if (!result)
        return;
    g_free(result->root);
    g_free(result->icon_path);
    g_free(result);
}

static void
idle_dispatch_free(IdleDispatch *dispatch)
{
    if (!dispatch)
        return;
    if (dispatch->destroy)
        dispatch->destroy(dispatch->user_data);
    g_free(dispatch->root);
    g_free(dispatch->icon_path);
    g_free(dispatch);
}

static void
ensure_tables(void)
{
    if (!icon_cache) {
        icon_cache = g_hash_table_new_full(g_str_hash, g_str_equal,
                                           g_free, g_free);
    }
    if (!pending) {
        pending = g_hash_table_new_full(g_str_hash, g_str_equal,
                                        g_free, (GDestroyNotify)g_ptr_array_unref);
    }
}

static gboolean
string_in_list_ci(const char *value, const char *const *list)
{
    if (!value)
        return FALSE;

    for (guint i = 0; list[i] != NULL; i++) {
        if (g_ascii_strcasecmp(value, list[i]) == 0)
            return TRUE;
    }

    return FALSE;
}

static gboolean
is_preferred_dir_name(const char *name)
{
    return string_in_list_ci(name, preferred_dirs);
}

static gboolean
is_pruned_dir_name(const char *name)
{
    return string_in_list_ci(name, pruned_dirs);
}

static gboolean
path_exists_regular(const char *path)
{
    return path && g_file_test(path, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_REGULAR);
}

static gboolean
is_emoji_only_dir(const char *path)
{
    const char *home = g_get_home_dir();

    if (!path || !path[0])
        return FALSE;
    if (home && strcmp(path, home) == 0)
        return TRUE;
    if (strcmp(path, "/") == 0)
        return TRUE;
    if (strcmp(path, "/tmp") == 0 || strcmp(path, "/var/tmp") == 0)
        return TRUE;
    if (strcmp(path, "/etc") == 0 || strcmp(path, "/usr") == 0 ||
        strcmp(path, "/var") == 0 || strcmp(path, "/opt") == 0 ||
        strcmp(path, "/dev") == 0 || strcmp(path, "/mnt") == 0 ||
        strcmp(path, "/media") == 0 || strcmp(path, "/srv") == 0 ||
        strcmp(path, "/home") == 0)
        return TRUE;

    return FALSE;
}

static char *
canonical_dir_path(const char *path)
{
    char *canonical;

    if (!path || !path[0])
        return NULL;

    canonical = g_canonicalize_filename(path, NULL);
    if (g_file_test(canonical, G_FILE_TEST_IS_DIR))
        return canonical;

    {
        char *dir = g_path_get_dirname(canonical);
        g_free(canonical);
        if (g_file_test(dir, G_FILE_TEST_IS_DIR))
            return dir;
        g_free(dir);
    }

    return NULL;
}

static gboolean
directory_has_project_marker(const char *dir_path)
{
    for (guint i = 0; project_markers[i] != NULL; i++) {
        char *candidate = g_build_filename(dir_path, project_markers[i], NULL);
        gboolean exists = g_file_test(candidate, G_FILE_TEST_EXISTS);
        g_free(candidate);
        if (exists)
            return TRUE;
    }

    return FALSE;
}

static gboolean
directory_has_icon_hint(const char *dir_path)
{
    for (guint i = 0; preferred_dirs[i] != NULL; i++) {
        char *candidate = g_build_filename(dir_path, preferred_dirs[i], NULL);
        gboolean exists = g_file_test(candidate, G_FILE_TEST_IS_DIR);
        g_free(candidate);
        if (exists)
            return TRUE;
    }

    for (guint i = 0; candidate_names[i] != NULL; i++) {
        char *candidate = g_build_filename(dir_path, candidate_names[i], NULL);
        gboolean exists = path_exists_regular(candidate);
        g_free(candidate);
        if (exists)
            return TRUE;
    }

    return FALSE;
}

char *
project_icon_cache_root_for_path(const char *path)
{
    char *dir = canonical_dir_path(path);
    char *fallback;
    char *hint_root = NULL;
    char *cursor;

    if (!dir)
        return NULL;
    if (is_emoji_only_dir(dir)) {
        g_free(dir);
        return NULL;
    }

    fallback = g_strdup(dir);
    cursor = g_strdup(dir);
    g_free(dir);

    for (;;) {
        char *parent;

        if (directory_has_project_marker(cursor)) {
            g_free(fallback);
            g_free(hint_root);
            return cursor;
        }
        if (!hint_root && directory_has_icon_hint(cursor))
            hint_root = g_strdup(cursor);

        parent = g_path_get_dirname(cursor);
        if (g_strcmp0(parent, cursor) == 0) {
            g_free(parent);
            g_free(cursor);
            if (hint_root) {
                g_free(fallback);
                return hint_root;
            }
            return fallback;
        }

        g_free(cursor);
        cursor = parent;
    }
}

static gboolean
is_image_extension(const char *name)
{
    const char *dot = strrchr(name ? name : "", '.');

    if (!dot)
        return FALSE;

    dot++;
    return g_ascii_strcasecmp(dot, "png") == 0 ||
           g_ascii_strcasecmp(dot, "svg") == 0 ||
           g_ascii_strcasecmp(dot, "ico") == 0 ||
           g_ascii_strcasecmp(dot, "webp") == 0;
}

static gboolean
is_candidate_name(const char *name)
{
    char *lower;
    gboolean match = FALSE;

    if (!name || !name[0])
        return FALSE;

    if (string_in_list_ci(name, candidate_names))
        return TRUE;

    lower = g_ascii_strdown(name, -1);
    match = g_str_has_prefix(lower, "favicon") ||
            g_str_has_prefix(lower, "icon") ||
            g_str_has_prefix(lower, "logo") ||
            g_str_has_prefix(lower, "apple-touch-icon");
    g_free(lower);
    return match;
}

static int
basename_rank(const char *name)
{
    for (guint i = 0; candidate_names[i] != NULL; i++) {
        if (g_ascii_strcasecmp(name, candidate_names[i]) == 0)
            return (int)i * 10;
    }

    if (is_candidate_name(name))
        return 140;

    return 220;
}

static int
path_rank(const char *full_path)
{
    char *lower;
    int rank = 0;

    lower = g_ascii_strdown(full_path ? full_path : "", -1);

    if (strstr(lower, "/public/"))
        rank -= 35;
    if (strstr(lower, "/assets/"))
        rank -= 28;
    if (strstr(lower, "/static/"))
        rank -= 20;
    if (strstr(lower, "/icons/") || strstr(lower, "/icon/"))
        rank -= 18;
    if (strstr(lower, "/img/") || strstr(lower, "/images/"))
        rank -= 14;
    if (strstr(lower, "/logo/") || strstr(lower, "/logos/"))
        rank -= 10;
    if (strstr(lower, "dark") || strstr(lower, "light"))
        rank += 8;

    g_free(lower);
    return rank;
}

static gboolean
score_candidate(const char *full_path, const char *name, int *score_out)
{
    int score;

    if (!full_path || !name || !is_image_extension(name))
        return FALSE;

    if (!is_candidate_name(name) && path_rank(full_path) >= 0)
        return FALSE;

    score = basename_rank(name) + path_rank(full_path);
    if (score_out)
        *score_out = score;
    return TRUE;
}

static void
consider_candidate(char **best_path,
                   int *best_score,
                   const char *full_path,
                   const char *name)
{
    int score;

    if (!score_candidate(full_path, name, &score))
        return;

    if (!*best_path || score < *best_score) {
        g_free(*best_path);
        *best_path = g_strdup(full_path);
        *best_score = score;
    }
}

static void
probe_candidate_dirs(const char *root, char **best_path, int *best_score)
{
    static const char *quick_dirs[] = {
        "",
        "public", "public/icons", "public/img", "public/images",
        "assets", "assets/icons", "assets/img", "assets/images",
        "static", "icons", "img", "images", "logo", "logos",
        NULL
    };

    for (guint di = 0; quick_dirs[di] != NULL; di++) {
        char *dir_path = quick_dirs[di][0]
            ? g_build_filename(root, quick_dirs[di], NULL)
            : g_strdup(root);

        if (!g_file_test(dir_path, G_FILE_TEST_IS_DIR)) {
            g_free(dir_path);
            continue;
        }

        for (guint ci = 0; candidate_names[ci] != NULL; ci++) {
            char *candidate = g_build_filename(dir_path, candidate_names[ci], NULL);
            if (path_exists_regular(candidate))
                consider_candidate(best_path, best_score, candidate, candidate_names[ci]);
            g_free(candidate);
        }

        g_free(dir_path);
    }
}

static char *
search_for_icon(const char *root)
{
    gint64 deadline = g_get_monotonic_time() + ICON_SEARCH_TIMEOUT_USEC;
    GQueue queue = G_QUEUE_INIT;
    char *best_path = NULL;
    int best_score = G_MAXINT;

    if (!root || !root[0] || !g_file_test(root, G_FILE_TEST_IS_DIR))
        return NULL;

    probe_candidate_dirs(root, &best_path, &best_score);
    if (best_path && best_score <= 10)
        return best_path;

    {
        DirQueueItem *item = g_new0(DirQueueItem, 1);
        item->path = g_strdup(root);
        item->depth = 0;
        item->preferred_context = TRUE;
        g_queue_push_tail(&queue, item);
    }

    while (!g_queue_is_empty(&queue)) {
        DirQueueItem *item = g_queue_pop_head(&queue);
        GDir *dir;
        const char *entry;

        if (g_get_monotonic_time() > deadline) {
            dir_queue_item_free(item);
            break;
        }

        dir = g_dir_open(item->path, 0, NULL);
        if (!dir) {
            dir_queue_item_free(item);
            continue;
        }

        while ((entry = g_dir_read_name(dir)) != NULL) {
            char *full_path;
            gboolean is_dir;
            gboolean preferred_child;

            if (g_get_monotonic_time() > deadline)
                break;
            if (entry[0] == '.')
                continue;

            full_path = g_build_filename(item->path, entry, NULL);
            is_dir = g_file_test(full_path, G_FILE_TEST_IS_DIR);
            preferred_child = item->preferred_context || is_preferred_dir_name(entry);

            if (is_dir) {
                gboolean allow_descend;

                if (is_pruned_dir_name(entry)) {
                    g_free(full_path);
                    continue;
                }

                allow_descend = item->depth < ICON_SEARCH_SHALLOW_DEPTH ||
                                preferred_child;
                if (allow_descend && item->depth + 1 <= ICON_SEARCH_MAX_DEPTH) {
                    DirQueueItem *child = g_new0(DirQueueItem, 1);
                    child->path = full_path;
                    child->depth = item->depth + 1;
                    child->preferred_context = preferred_child;
                    g_queue_push_tail(&queue, child);
                    continue;
                }
            } else if (item->preferred_context || item->depth <= 1 ||
                       is_candidate_name(entry)) {
                consider_candidate(&best_path, &best_score, full_path, entry);
            }

            g_free(full_path);
        }

        g_dir_close(dir);
        dir_queue_item_free(item);
    }

    while (!g_queue_is_empty(&queue))
        dir_queue_item_free(g_queue_pop_head(&queue));

    return best_path;
}

static gboolean
dispatch_idle_cb(gpointer user_data)
{
    IdleDispatch *dispatch = user_data;

    if (dispatch->callback)
        dispatch->callback(dispatch->root, dispatch->icon_path, dispatch->user_data);

    idle_dispatch_free(dispatch);
    return G_SOURCE_REMOVE;
}

static void
schedule_dispatch(const char *root,
                  const char *icon_path,
                  ProjectIconResolvedFunc callback,
                  gpointer user_data,
                  GDestroyNotify destroy)
{
    IdleDispatch *dispatch;

    dispatch = g_new0(IdleDispatch, 1);
    dispatch->root = g_strdup(root);
    dispatch->icon_path = g_strdup(icon_path ? icon_path : "");
    dispatch->callback = callback;
    dispatch->user_data = user_data;
    dispatch->destroy = destroy;
    g_idle_add(dispatch_idle_cb, dispatch);
}

const char *
project_icon_cache_lookup(const char *root)
{
    const char *value;

    ensure_tables();

    if (!root || !root[0])
        return NULL;

    value = g_hash_table_lookup(icon_cache, root);
    if (!value)
        return NULL;

    if (value[0] == '\0')
        return NULL;

    if (!path_exists_regular(value)) {
        g_hash_table_remove(icon_cache, root);
        return NULL;
    }

    return value;
}

const char *
project_icon_cache_lookup_for_path(const char *path)
{
    const char *value;
    char *root = project_icon_cache_root_for_path(path);

    if (!root)
        return NULL;

    value = project_icon_cache_lookup(root);
    g_free(root);
    return value;
}

static void
icon_search_thread(GTask *task,
                   gpointer source_object,
                   gpointer task_data,
                   GCancellable *cancellable)
{
    SearchResult *result;
    const char *root = task_data;

    (void)source_object;
    (void)cancellable;

    result = g_new0(SearchResult, 1);
    result->root = g_strdup(root);
    result->icon_path = search_for_icon(root);
    g_task_return_pointer(task, result, (GDestroyNotify)search_result_free);
}

static void
icon_search_complete(GObject *source_object,
                     GAsyncResult *result,
                     gpointer user_data)
{
    GTask *task = G_TASK(result);
    SearchResult *search;
    GPtrArray *listeners;
    const char *stored_icon;
    gpointer pending_key = NULL;
    gpointer pending_value = NULL;

    (void)source_object;
    (void)user_data;

    search = g_task_propagate_pointer(task, NULL);
    if (!search)
        return;

    ensure_tables();
    g_hash_table_replace(icon_cache,
                         g_strdup(search->root),
                         g_strdup(search->icon_path ? search->icon_path : ""));

    listeners = NULL;
    if (g_hash_table_steal_extended(pending, search->root,
                                    &pending_key, &pending_value)) {
        listeners = pending_value;
        g_free(pending_key);
    }

    stored_icon = project_icon_cache_lookup(search->root);
    if (listeners) {
        for (guint i = 0; i < listeners->len; i++) {
            PendingListener *listener = g_ptr_array_index(listeners, i);
            if (listener->callback) {
                listener->callback(search->root, stored_icon, listener->user_data);
                listener->user_data = NULL;
            }
        }
        g_ptr_array_unref(listeners);
    }

    search_result_free(search);
}

void
project_icon_cache_request(const char *path,
                           ProjectIconResolvedFunc callback,
                           gpointer user_data,
                           GDestroyNotify destroy)
{
    char *root;
    const char *cached;
    GPtrArray *listeners;
    PendingListener *listener;
    GTask *task;

    if (!callback) {
        if (destroy)
            destroy(user_data);
        return;
    }

    root = project_icon_cache_root_for_path(path);
    if (!root) {
        schedule_dispatch("", NULL, callback, user_data, destroy);
        return;
    }

    ensure_tables();

    cached = project_icon_cache_lookup(root);
    if (cached || g_hash_table_contains(icon_cache, root)) {
        schedule_dispatch(root, cached, callback, user_data, destroy);
        g_free(root);
        return;
    }

    listeners = g_hash_table_lookup(pending, root);
    if (!listeners) {
        listeners = g_ptr_array_new_with_free_func(pending_listener_free);
        g_hash_table_insert(pending, g_strdup(root), listeners);

        task = g_task_new(NULL, NULL, icon_search_complete, NULL);
        g_task_set_task_data(task, g_strdup(root), g_free);
        g_task_run_in_thread(task, icon_search_thread);
        g_object_unref(task);
    }

    listener = g_new0(PendingListener, 1);
    listener->callback = callback;
    listener->user_data = user_data;
    listener->destroy = destroy;
    g_ptr_array_add(listeners, listener);

    g_free(root);
}

void
project_icon_cache_restore_entry(const char *root, const char *icon_path)
{
    ensure_tables();

    if (!root || !root[0])
        return;
    if (is_emoji_only_dir(root))
        return;

    g_hash_table_replace(icon_cache,
                         g_strdup(root),
                         g_strdup(path_exists_regular(icon_path) ? icon_path : ""));
}

void
project_icon_cache_foreach(ProjectIconCacheForeachFunc func, gpointer user_data)
{
    GHashTableIter iter;
    gpointer key, value;

    ensure_tables();

    if (!func)
        return;

    g_hash_table_iter_init(&iter, icon_cache);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        const char *root = key;
        const char *icon_path = value;

        if (!icon_path || icon_path[0] == '\0')
            continue;
        if (!path_exists_regular(icon_path))
            continue;

        func(root, icon_path, user_data);
    }
}
