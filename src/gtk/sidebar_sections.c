#include "sidebar_sections.h"

#include <gtk/gtk.h>
#include <string.h>

#include "workspace.h"

static void
sidebar_sections_clear_box_children(GtkWidget *box)
{
    GtkWidget *child;

    if (!GTK_IS_BOX(box))
        return;

    child = gtk_widget_get_first_child(box);
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_box_remove(GTK_BOX(box), child);
        child = next;
    }
}

static void
sidebar_sections_hide_label(GtkWidget *section_label)
{
    if (!GTK_IS_LABEL(section_label))
        return;

    gtk_label_set_text(GTK_LABEL(section_label), "");
    gtk_widget_set_visible(section_label, FALSE);
    gtk_widget_set_tooltip_text(section_label, NULL);
}

static void
sidebar_sections_format_status_age(const workspace_status_entry *entry,
                                   char *buf,
                                   gsize bufsz)
{
    gint64 age_sec;

    if (!entry || entry->updated_at_usec <= 0 || !buf || bufsz == 0) {
        if (buf && bufsz > 0)
            buf[0] = '\0';
        return;
    }

    age_sec = (g_get_real_time() - entry->updated_at_usec) / G_USEC_PER_SEC;
    if (age_sec < 10) {
        g_strlcpy(buf, "now", bufsz);
    } else if (age_sec < 60) {
        g_snprintf(buf, bufsz, "%" G_GINT64_FORMAT "s", age_sec);
    } else if (age_sec < 3600) {
        g_snprintf(buf, bufsz, "%" G_GINT64_FORMAT "m", age_sec / 60);
    } else if (age_sec < 86400) {
        g_snprintf(buf, bufsz, "%" G_GINT64_FORMAT "h", age_sec / 3600);
    } else {
        g_snprintf(buf, bufsz, "%" G_GINT64_FORMAT "d", age_sec / 86400);
    }
}

static void
sidebar_sections_compact_cwd(const char *cwd, char *buf, gsize bufsz)
{
    const char *home;
    gsize homelen;
    const char *display;
    char tilde_path[256];
    const char *last_slash;
    const char *last;

    if (!buf || bufsz == 0)
        return;
    if (!cwd || !cwd[0]) {
        buf[0] = '\0';
        return;
    }

    home = g_get_home_dir();
    homelen = home ? strlen(home) : 0;
    display = cwd;

    if (home && homelen > 0 && strncmp(cwd, home, homelen) == 0) {
        g_snprintf(tilde_path, sizeof(tilde_path), "~%s", cwd + homelen);
        display = tilde_path;
    }

    if (strlen(display) <= 26) {
        g_strlcpy(buf, display, bufsz);
        return;
    }

    last_slash = strrchr(display, '/');
    last = last_slash ? last_slash + 1 : display;
    if (!*last && last_slash > display) {
        const char *p = last_slash - 1;
        while (p > display && *p != '/')
            p--;
        if (*p == '/')
            p++;
        last = p;
    }

    if (last && last[0])
        g_snprintf(buf, bufsz, "~/.../%.20s", last);
    else
        g_snprintf(buf, bufsz, "%.25s", display);
}

void
sidebar_ui_build_notification_preview_section(GtkWidget *section_label,
                                              const char *preview,
                                              gboolean enabled)
{
    g_autofree char *single_line = NULL;
    gsize i;

    if (!GTK_IS_LABEL(section_label))
        return;

    if (!enabled || !preview || !preview[0]) {
        gtk_label_set_text(GTK_LABEL(section_label), "");
        gtk_widget_set_visible(section_label, FALSE);
        gtk_widget_set_tooltip_text(section_label, NULL);
        return;
    }

    single_line = g_strdup(preview);
    g_strstrip(single_line);
    for (i = 0; single_line[i] != '\0'; i++) {
        if (single_line[i] == '\r' || single_line[i] == '\n')
            single_line[i] = ' ';
    }

    if (!single_line[0]) {
        gtk_label_set_text(GTK_LABEL(section_label), "");
        gtk_widget_set_visible(section_label, FALSE);
        gtk_widget_set_tooltip_text(section_label, NULL);
        return;
    }

    gtk_label_set_text(GTK_LABEL(section_label), single_line);
    gtk_widget_set_visible(section_label, TRUE);
    gtk_widget_set_tooltip_text(section_label, single_line);
}

void
sidebar_ui_build_branch_cwd_section(GtkWidget *section_label,
                                    const char *cwd,
                                    const char *branch,
                                    gboolean enabled)
{
    char short_cwd[48];
    char line_buf[192];
    gboolean has_branch = branch && branch[0];

    if (!GTK_IS_LABEL(section_label))
        return;

    if (!enabled) {
        gtk_label_set_text(GTK_LABEL(section_label), "");
        gtk_widget_set_visible(section_label, FALSE);
        gtk_widget_set_tooltip_text(section_label, NULL);
        return;
    }

    sidebar_sections_compact_cwd(cwd, short_cwd, sizeof(short_cwd));
    if (short_cwd[0] && has_branch)
        g_snprintf(line_buf, sizeof(line_buf), "%s [%s]", short_cwd, branch);
    else if (short_cwd[0])
        g_snprintf(line_buf, sizeof(line_buf), "%s", short_cwd);
    else if (has_branch)
        g_snprintf(line_buf, sizeof(line_buf), "[%s]", branch);
    else
        line_buf[0] = '\0';

    gtk_label_set_text(GTK_LABEL(section_label), line_buf);
    gtk_widget_set_visible(section_label, line_buf[0] != '\0');
    gtk_widget_set_tooltip_text(section_label,
                                (cwd && cwd[0]) ? cwd : NULL);
}

void
sidebar_ui_build_progress_section(GtkWidget *section_label,
                                  int progress_state,
                                  int progress_percent,
                                  gboolean enabled)
{
    char line_buf[64];
    char bar[5];
    int filled;

    if (!GTK_IS_LABEL(section_label))
        return;

    if (!enabled || progress_state <= 0) {
        sidebar_sections_hide_label(section_label);
        return;
    }

    if (progress_percent < 0) {
        if (progress_state == 2) {
            gtk_label_set_text(GTK_LABEL(section_label), "p! [....]");
            gtk_widget_set_tooltip_text(section_label, "Progress error");
        } else if (progress_state == 4) {
            gtk_label_set_text(GTK_LABEL(section_label), "p|| [....]");
            gtk_widget_set_tooltip_text(section_label, "Progress paused");
        } else if (progress_state == 3) {
            gtk_label_set_text(GTK_LABEL(section_label), "p~ [....]");
            gtk_widget_set_tooltip_text(section_label,
                                        "Progress indeterminate");
        } else {
            gtk_label_set_text(GTK_LABEL(section_label), "p [....]");
            gtk_widget_set_tooltip_text(section_label, "Progress");
        }
        gtk_widget_set_visible(section_label, TRUE);
        return;
    }

    progress_percent = CLAMP(progress_percent, 0, 100);
    filled = (progress_percent + 24) / 25;
    for (int i = 0; i < 4; i++)
        bar[i] = (i < filled) ? '#' : '-';
    bar[4] = '\0';

    if (progress_state == 4) {
        g_snprintf(line_buf, sizeof(line_buf), "p|| [%s] %d%%",
                   bar, progress_percent);
        gtk_widget_set_tooltip_text(section_label, "Progress paused");
    } else if (progress_state == 2) {
        g_snprintf(line_buf, sizeof(line_buf), "p! [%s] %d%%",
                   bar, progress_percent);
        gtk_widget_set_tooltip_text(section_label, "Progress error");
    } else if (progress_state == 3) {
        g_snprintf(line_buf, sizeof(line_buf), "p~ [%s] %d%%",
                   bar, progress_percent);
        gtk_widget_set_tooltip_text(section_label, "Progress indeterminate");
    } else {
        g_snprintf(line_buf, sizeof(line_buf), "p [%s] %d%%",
                   bar, progress_percent);
        gtk_widget_set_tooltip_text(section_label, "Progress");
    }

    gtk_label_set_text(GTK_LABEL(section_label), line_buf);
    gtk_widget_set_visible(section_label, TRUE);
}

void
sidebar_ui_build_structure_indicator_section(GtkWidget *section_label,
                                             gboolean strip_mode,
                                             int pane_or_column_count,
                                             int tab_count,
                                             gboolean enabled)
{
    char line_buf[48];

    if (!GTK_IS_LABEL(section_label))
        return;

    if (!enabled || (pane_or_column_count <= 1 && tab_count <= 1)) {
        sidebar_sections_hide_label(section_label);
        return;
    }

    pane_or_column_count = MAX(pane_or_column_count, 1);
    tab_count = MAX(tab_count, 0);
    if (tab_count > 0) {
        g_snprintf(line_buf, sizeof(line_buf), "%c%d T%d",
                   strip_mode ? 'C' : 'P',
                   pane_or_column_count,
                   tab_count);
    } else {
        g_snprintf(line_buf, sizeof(line_buf), "%c%d",
                   strip_mode ? 'C' : 'P',
                   pane_or_column_count);
    }

    gtk_label_set_text(GTK_LABEL(section_label), line_buf);
    gtk_widget_set_visible(section_label, TRUE);
    gtk_widget_set_tooltip_text(section_label,
                                strip_mode ? "Columns / tabs"
                                           : "Panes / tabs");
}

void
sidebar_ui_build_workspace_status_section(GtkWidget *section_box,
                                          GPtrArray *status_entries,
                                          int max_entries)
{
    int show_count;

    if (!GTK_IS_BOX(section_box))
        return;

    sidebar_sections_clear_box_children(section_box);

    if (!status_entries || status_entries->len == 0 || max_entries <= 0) {
        gtk_widget_set_visible(section_box, FALSE);
        return;
    }

    show_count = MIN((int)status_entries->len, max_entries);
    for (int i = 0; i < show_count; i++) {
        workspace_status_entry *entry = g_ptr_array_index(status_entries, i);
        const char *provider;
        const char *summary;
        GtkWidget *label;
        char age_buf[16];
        char line_buf[512];

        if (!entry)
            continue;

        provider = entry->provider[0] ? entry->provider : "agent";
        summary = entry->summary[0]
            ? entry->summary
            : (entry->status[0] ? entry->status : entry->kind);
        sidebar_sections_format_status_age(entry, age_buf, sizeof(age_buf));

        if (entry->status[0] && entry->summary[0] &&
            g_strcmp0(entry->status, entry->summary) != 0) {
            if (age_buf[0]) {
                g_snprintf(line_buf, sizeof(line_buf), "%s %s: %s [%s]",
                           provider, entry->status, entry->summary, age_buf);
            } else {
                g_snprintf(line_buf, sizeof(line_buf), "%s %s: %s",
                           provider, entry->status, entry->summary);
            }
        } else if (age_buf[0]) {
            g_snprintf(line_buf, sizeof(line_buf), "%s: %s [%s]",
                       provider, summary ? summary : "", age_buf);
        } else {
            g_snprintf(line_buf, sizeof(line_buf), "%s: %s",
                       provider, summary ? summary : "");
        }

        label = gtk_label_new(line_buf);
        gtk_widget_add_css_class(label, "sidebar-status");
        gtk_widget_add_css_class(label, "sidebar-status-entry");
        gtk_label_set_xalign(GTK_LABEL(label), 0);
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(label), 30);
        gtk_widget_set_visible(label, TRUE);
        if (entry->detail[0])
            gtk_widget_set_tooltip_text(label, entry->detail);
        if (entry->attention)
            gtk_widget_add_css_class(label, "has-activity");
        gtk_box_append(GTK_BOX(section_box), label);
    }

    if ((int)status_entries->len > show_count) {
        GtkWidget *more = gtk_label_new(NULL);
        char more_buf[48];

        g_snprintf(more_buf, sizeof(more_buf), "+%d more",
                   (int)status_entries->len - show_count);
        gtk_label_set_text(GTK_LABEL(more), more_buf);
        gtk_widget_add_css_class(more, "sidebar-status");
        gtk_label_set_xalign(GTK_LABEL(more), 0);
        gtk_widget_set_visible(more, TRUE);
        gtk_box_append(GTK_BOX(section_box), more);
    }

    gtk_widget_set_visible(section_box, TRUE);
}
