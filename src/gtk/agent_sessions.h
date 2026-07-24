#pragma once

#include <gtk/gtk.h>

typedef struct {
    char *provider;
    char *session_id;
    char *cwd;
    char *title;
    gint64 updated_at;
} AgentSession;

GPtrArray *agent_sessions_load(const char *home_dir, guint per_provider_limit);
void agent_session_free(gpointer data);

GtkWidget *agent_sessions_panel_new(void);
void agent_sessions_panel_refresh(GtkWidget *panel);
