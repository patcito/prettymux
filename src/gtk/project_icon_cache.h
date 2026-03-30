#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef void (*ProjectIconResolvedFunc)(const char *root,
                                        const char *icon_path,
                                        gpointer user_data);
typedef void (*ProjectIconCacheForeachFunc)(const char *root,
                                            const char *icon_path,
                                            gpointer user_data);

char *project_icon_cache_root_for_path(const char *path);
const char *project_icon_cache_lookup(const char *root);
const char *project_icon_cache_lookup_for_path(const char *path);

void project_icon_cache_request(const char *path,
                                ProjectIconResolvedFunc callback,
                                gpointer user_data,
                                GDestroyNotify destroy);

void project_icon_cache_restore_entry(const char *root, const char *icon_path);
void project_icon_cache_foreach(ProjectIconCacheForeachFunc func,
                                gpointer user_data);

G_END_DECLS
