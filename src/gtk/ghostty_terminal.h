/*
 * ghostty_terminal.h - GObject widget wrapping ghostty's embedded C API
 *
 * A GtkWidget subclass (composite widget containing a GtkGLArea) that
 * creates and manages a ghostty terminal surface with OpenGL rendering.
 */
#pragma once

#include <sys/types.h>
#include <gtk/gtk.h>
#include "ghostty.h"

G_BEGIN_DECLS

#define GHOSTTY_TYPE_TERMINAL (ghostty_terminal_get_type())
G_DECLARE_FINAL_TYPE(GhosttyTerminal, ghostty_terminal, GHOSTTY, TERMINAL, GtkWidget)

/* The global ghostty app handle, defined in main.c */
extern ghostty_app_t g_ghostty_app;
extern float g_ghostty_default_font_size;

/*
 * ghostty_terminal_new:
 * @start_cwd: (nullable): initial working directory for the shell, or NULL
 *             for the user's home directory.
 *
 * Returns: a new #GhosttyTerminal widget.
 */
GtkWidget *ghostty_terminal_new(const char *start_cwd);

/*
 * ghostty_terminal_get_surface:
 *
 * Returns the underlying ghostty_surface_t handle, or NULL if the surface
 * has not been realized yet.
 */
ghostty_surface_t ghostty_terminal_get_surface(GhosttyTerminal *self);

/*
 * ghostty_terminal_get_title:
 *
 * Returns the most recently reported terminal title, or NULL.
 * The string is owned by the widget.
 */
const char *ghostty_terminal_get_title(GhosttyTerminal *self);

/*
 * ghostty_terminal_get_cwd:
 *
 * Returns the most recently reported working directory, or NULL.
 * The string is owned by the widget.
 */
const char *ghostty_terminal_get_cwd(GhosttyTerminal *self);
const char *ghostty_terminal_get_id(GhosttyTerminal *self);
pid_t ghostty_terminal_get_session_id(GhosttyTerminal *self);
const char *ghostty_terminal_get_tty_name(GhosttyTerminal *self);
const char *ghostty_terminal_get_tty_path(GhosttyTerminal *self);

/*
 * ghostty_terminal_get_exit_code:
 *
 * Returns the exit code of the child process once it has exited,
 * or -1 if the process is still running.
 */
int ghostty_terminal_get_exit_code(GhosttyTerminal *self);

/*
 * ghostty_terminal_set_title:
 * ghostty_terminal_set_cwd:
 *
 * Called from the global action callback to push state into the widget
 * and emit the corresponding signal.
 */
void ghostty_terminal_set_title(GhosttyTerminal *self, const char *title);
void ghostty_terminal_set_cwd(GhosttyTerminal *self, const char *cwd);
void ghostty_terminal_set_scope(GhosttyTerminal *self,
                                pid_t            session_id,
                                const char      *tty_name,
                                const char      *tty_path);
void ghostty_terminal_notify_bell(GhosttyTerminal *self);
void ghostty_terminal_notify_command_finished(GhosttyTerminal *self,
                                              int exit_code,
                                              uint64_t duration_ns);
void ghostty_terminal_notify_child_exited(GhosttyTerminal *self,
                                          uint32_t exit_code);
void ghostty_terminal_set_dummy_target(GhosttyTerminal *self, GtkWidget *dummy);
GtkWidget *ghostty_terminal_get_dummy_target(GhosttyTerminal *self);

/*
 * Give keyboard focus to the terminal's GtkGLArea.
 */
void ghostty_terminal_focus(GhosttyTerminal *self);
void ghostty_terminal_send_key_event(GhosttyTerminal *self,
                                     guint            keyval,
                                     guint            keycode,
                                     GdkModifierType  state);

/*
 * ghostty_terminal_queue_render:
 *
 * Queue a render on this terminal's GtkGLArea.  Called from the global
 * action callback when ghostty reports new content (GHOSTTY_ACTION_RENDER).
 */
void ghostty_terminal_queue_render(GhosttyTerminal *self);

/*
 * Activity tracking: mark terminal as having new unread output.
 * Cleared when the terminal's tab becomes focused/selected.
 */
void     ghostty_terminal_mark_activity(GhosttyTerminal *self);
void     ghostty_terminal_clear_activity(GhosttyTerminal *self);
gboolean ghostty_terminal_has_activity(GhosttyTerminal *self);

/*
 * Progress bar state (from OSC 9;4 / GHOSTTY_ACTION_PROGRESS_REPORT).
 *   state:   0=remove, 1=set, 2=error, 3=indeterminate, 4=pause  (-1 = none)
 *   percent: 0-100
 */
void ghostty_terminal_set_progress(GhosttyTerminal *self, int state, int percent);
int  ghostty_terminal_get_progress_percent(GhosttyTerminal *self);
int  ghostty_terminal_get_progress_state(GhosttyTerminal *self);

/*
 * Status bar: thin bar below the terminal showing CWD + git branch.
 * Called when CWD or git branch changes.
 */
void ghostty_terminal_set_status(GhosttyTerminal *self,
                                 const char *cwd,
                                 const char *git_branch);

/*
 * Signals:
 *   "title-changed"      (GhosttyTerminal *self, const char *title)
 *   "pwd-changed"        (GhosttyTerminal *self, const char *cwd)
 *   "command-finished"   (GhosttyTerminal *self, int exit_code, guint64 duration_ns)
 *   "bell"               (GhosttyTerminal *self)
 *   "process-exited"     (GhosttyTerminal *self, int exit_code)
 *   "close-requested"    (GhosttyTerminal *self)
 */

G_END_DECLS
