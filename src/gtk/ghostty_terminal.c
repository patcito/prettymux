/*
 * ghostty_terminal.c - GObject widget wrapping ghostty's embedded C API
 *
 * Composite widget: GtkWidget containing a GtkGLArea that hosts a ghostty
 * terminal surface.  Forwards GL render/resize, keyboard, mouse, scroll,
 * focus, and IME events to ghostty.  Runs a 16 ms tick timer for
 * ghostty_app_tick().
 */

#include "ghostty_terminal.h"
#include "socket_server.h"

#include <gdk/gdk.h>
#include <stdlib.h>
#include <string.h>

/* ── Signal IDs ────────────────────────────────────────────────── */

enum {
    SIGNAL_TITLE_CHANGED,
    SIGNAL_PWD_CHANGED,
    SIGNAL_COMMAND_FINISHED,
    SIGNAL_BELL,
    SIGNAL_PROCESS_EXITED,
    SIGNAL_CLOSE_REQUESTED,
    N_SIGNALS,
};

static guint signals[N_SIGNALS];

/* ── Private structure ─────────────────────────────────────────── */

struct _GhosttyTerminal {
    GtkWidget parent_instance;

    GtkGLArea         *gl_area;
    ghostty_surface_t  surface;
    guint              tick_source_id;
    gboolean           exit_emitted;

    /* IME */
    GtkIMContext      *im_context;

    /* Cached state pushed from action callbacks */
    char              *title;
    char              *cwd;
    char              *start_cwd;
    int                exit_code; /* -1 while running */

    /* Activity tracking */
    gboolean           has_new_output;

    /* Progress bar (OSC 9;4) */
    int                progress_state;   /* -1=none, 0=remove, 1=set, 2=error, 3=indeterminate, 4=pause */
    int                progress_percent; /* 0-100, or -1 when no progress */
};

G_DEFINE_FINAL_TYPE(GhosttyTerminal, ghostty_terminal, GTK_TYPE_WIDGET)

/* ── Helpers ───────────────────────────────────────────────────── */

static ghostty_input_mods_e
translate_mods(GdkModifierType mods)
{
    int r = GHOSTTY_MODS_NONE;
    if (mods & GDK_SHIFT_MASK)   r |= GHOSTTY_MODS_SHIFT;
    if (mods & GDK_CONTROL_MASK) r |= GHOSTTY_MODS_CTRL;
    if (mods & GDK_ALT_MASK)     r |= GHOSTTY_MODS_ALT;
    if (mods & GDK_SUPER_MASK)   r |= GHOSTTY_MODS_SUPER;
    return (ghostty_input_mods_e)r;
}

static ghostty_input_mouse_button_e
translate_button(guint button)
{
    switch (button) {
    case 1:  return GHOSTTY_MOUSE_LEFT;
    case 2:  return GHOSTTY_MOUSE_MIDDLE;
    case 3:  return GHOSTTY_MOUSE_RIGHT;
    case 4:  return GHOSTTY_MOUSE_FOUR;
    case 5:  return GHOSTTY_MOUSE_FIVE;
    default: return GHOSTTY_MOUSE_LEFT;
    }
}

/* ── GL callbacks ──────────────────────────────────────────────── */

static void
on_gl_realize(GtkGLArea *area, gpointer user_data)
{
    GhosttyTerminal *self = GHOSTTY_TERMINAL(user_data);

    gtk_gl_area_make_current(area);
    if (gtk_gl_area_get_error(area) != NULL) {
        return;
    }
    if (!g_ghostty_app) {
        return;
    }

    ghostty_surface_config_s config = ghostty_surface_config_new();
    config.platform_tag = GHOSTTY_PLATFORM_LINUX;
    config.platform.gtk.gtk_widget = (void *)self->gl_area;

    double scale = gtk_widget_get_scale_factor(GTK_WIDGET(area));
    config.scale_factor = scale;

    const char *home = g_get_home_dir();
    config.working_directory = (self->start_cwd && self->start_cwd[0])
                                   ? self->start_cwd
                                   : home;

    /* Shell integration env vars */
    ghostty_env_var_s env_vars[3];
    size_t env_count = 0;

    env_vars[env_count].key = "PRETTYMUX";
    env_vars[env_count].value = "1";
    env_count++;

    const char *sock_path = socket_server_get_path();
    if (sock_path) {
        env_vars[env_count].key = "PRETTYMUX_SOCKET";
        env_vars[env_count].value = sock_path;
        env_count++;
    }

    const char *bash_env = g_getenv("BASH_ENV");
    if (bash_env) {
        env_vars[env_count].key = "BASH_ENV";
        env_vars[env_count].value = bash_env;
        env_count++;
    }

    config.env_vars = env_vars;
    config.env_var_count = env_count;

    self->surface = ghostty_surface_new(g_ghostty_app, &config);
    if (self->surface) {
        ghostty_surface_init_opengl(self->surface);

        /* Push initial geometry so the surface isn't stuck at 0x0 */
        ghostty_surface_set_content_scale(self->surface, scale, scale);
        int w = gtk_widget_get_width(GTK_WIDGET(area));
        int h = gtk_widget_get_height(GTK_WIDGET(area));
        if (w > 0 && h > 0)
            ghostty_surface_set_size(self->surface, (uint32_t)w, (uint32_t)h);

        gboolean focused = gtk_widget_has_focus(GTK_WIDGET(self->gl_area));
        ghostty_surface_set_focus(self->surface, focused);

        /* Queue first render */
        gtk_gl_area_queue_render(area);
    }
}

static gboolean
on_gl_render(GtkGLArea *area, GdkGLContext *context, gpointer user_data)
{
    (void)context;
    GhosttyTerminal *self = GHOSTTY_TERMINAL(user_data);

    if (!self->surface)
        return FALSE;

    gtk_gl_area_make_current(area);
    ghostty_surface_draw_frame(self->surface);
    return TRUE;
}

static void
on_gl_resize(GtkGLArea *area, int width, int height, gpointer user_data)
{
    GhosttyTerminal *self = GHOSTTY_TERMINAL(user_data);

    if (!self->surface)
        return;

    double scale = gtk_widget_get_scale_factor(GTK_WIDGET(area));
    ghostty_surface_set_content_scale(self->surface, scale, scale);
    /* GtkGLArea resize callback gives pixel dimensions already */
    ghostty_surface_set_size(self->surface, (uint32_t)width, (uint32_t)height);
}

/* ── Tick timer ────────────────────────────────────────────────── */

static gboolean
tick_callback(gpointer user_data)
{
    GhosttyTerminal *self = GHOSTTY_TERMINAL(user_data);

    if (g_ghostty_app)
        ghostty_app_tick(g_ghostty_app);

    gtk_gl_area_queue_render(self->gl_area);

    /* Check for process exit */
    if (self->surface && ghostty_surface_process_exited(self->surface)
        && !self->exit_emitted) {
        self->exit_emitted = TRUE;
        g_signal_emit(self, signals[SIGNAL_PROCESS_EXITED], 0, self->exit_code);
    }

    return G_SOURCE_CONTINUE;
}

/* ── Keyboard ──────────────────────────────────────────────────── */

static gboolean
on_key_pressed(GtkEventControllerKey *controller,
               guint                  keyval,
               guint                  keycode,
               GdkModifierType        state,
               gpointer               user_data)
{
    (void)controller;
    GhosttyTerminal *self = GHOSTTY_TERMINAL(user_data);

    if (!self->surface)
        return FALSE;

    /* Let IME have first crack */
    if (gtk_im_context_filter_keypress(self->im_context,
            gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(controller))))
        return TRUE;

    ghostty_input_key_s ke = {0};
    ke.action = GHOSTTY_ACTION_PRESS;
    ke.keycode = keycode; /* XKB hardware keycode from GDK */
    ke.mods = translate_mods(state);
    ke.composing = false;

    /* For plain keys (no Ctrl/Alt/Super), send text */
    if (!(state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK))) {
        gunichar uc = gdk_keyval_to_unicode(keyval);
        if (uc >= 0x20 && uc != 0) {
            char buf[8];
            int len = g_unichar_to_utf8(uc, buf);
            buf[len] = '\0';
            ke.text = buf;
        }
    }

    /* Always set unshifted_codepoint */
    guint lower = gdk_keyval_to_lower(keyval);
    gunichar cp = gdk_keyval_to_unicode(lower);
    ke.unshifted_codepoint = (cp < 0x110000) ? cp : 0;

    ghostty_surface_key(self->surface, ke);
    /* Immediate tick + render so cursor position updates without lag */
    if (g_ghostty_app)
        ghostty_app_tick(g_ghostty_app);
    gtk_gl_area_queue_render(self->gl_area);
    return TRUE;
}

static void
on_key_released(GtkEventControllerKey *controller,
                guint                  keyval,
                guint                  keycode,
                GdkModifierType        state,
                gpointer               user_data)
{
    (void)controller;
    (void)keyval;
    GhosttyTerminal *self = GHOSTTY_TERMINAL(user_data);

    if (!self->surface)
        return;

    ghostty_input_key_s ke = {0};
    ke.action = GHOSTTY_ACTION_RELEASE;
    ke.keycode = keycode;
    ke.mods = translate_mods(state);
    ghostty_surface_key(self->surface, ke);
}

/* ── IME ───────────────────────────────────────────────────────── */

static void
on_im_commit(GtkIMContext *im, const char *text, gpointer user_data)
{
    GhosttyTerminal *self = GHOSTTY_TERMINAL(user_data);
    if (self->surface && text && *text) {
        ghostty_surface_text(self->surface, text, strlen(text));
        if (g_ghostty_app)
            ghostty_app_tick(g_ghostty_app);
        gtk_gl_area_queue_render(self->gl_area);
    }
}

static void
on_im_preedit_changed(GtkIMContext *im, gpointer user_data)
{
    GhosttyTerminal *self = GHOSTTY_TERMINAL(user_data);
    if (!self->surface)
        return;

    char *preedit_str = NULL;
    PangoAttrList *attrs = NULL;
    int cursor_pos = 0;
    gtk_im_context_get_preedit_string(im, &preedit_str, &attrs, &cursor_pos);

    if (preedit_str) {
        ghostty_surface_preedit(self->surface, preedit_str, strlen(preedit_str));
        g_free(preedit_str);
    }
    if (attrs)
        pango_attr_list_unref(attrs);
}

/* ── Mouse ─────────────────────────────────────────────────────── */

static void
on_click_pressed(GtkGestureClick *gesture,
                 int              n_press,
                 double           x,
                 double           y,
                 gpointer         user_data)
{
    (void)n_press;
    GhosttyTerminal *self = GHOSTTY_TERMINAL(user_data);

    if (!self->surface)
        return;

    gtk_widget_grab_focus(GTK_WIDGET(self->gl_area));

    GdkModifierType state = gtk_event_controller_get_current_event_state(
        GTK_EVENT_CONTROLLER(gesture));
    guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));

    ghostty_surface_mouse_pos(self->surface, x, y, translate_mods(state));
    ghostty_surface_mouse_button(self->surface, GHOSTTY_MOUSE_PRESS,
                                 translate_button(button), translate_mods(state));
    gtk_gl_area_queue_render(self->gl_area);
}

static void
on_click_released(GtkGestureClick *gesture,
                  int              n_press,
                  double           x,
                  double           y,
                  gpointer         user_data)
{
    (void)n_press;
    GhosttyTerminal *self = GHOSTTY_TERMINAL(user_data);

    if (!self->surface)
        return;

    GdkModifierType state = gtk_event_controller_get_current_event_state(
        GTK_EVENT_CONTROLLER(gesture));
    guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));

    ghostty_surface_mouse_pos(self->surface, x, y, translate_mods(state));
    ghostty_surface_mouse_button(self->surface, GHOSTTY_MOUSE_RELEASE,
                                 translate_button(button), translate_mods(state));
    gtk_gl_area_queue_render(self->gl_area);
}

static void
on_motion(GtkEventControllerMotion *controller,
          double                    x,
          double                    y,
          gpointer                  user_data)
{
    GhosttyTerminal *self = GHOSTTY_TERMINAL(user_data);

    if (!self->surface)
        return;

    GdkModifierType state = gtk_event_controller_get_current_event_state(
        GTK_EVENT_CONTROLLER(controller));
    ghostty_surface_mouse_pos(self->surface, x, y, translate_mods(state));
    gtk_gl_area_queue_render(self->gl_area);
}

/* ── Focus on hover ───────────────────────────────────────────── */

static void
on_mouse_enter(GtkEventControllerMotion *controller,
               double x, double y,
               gpointer user_data)
{
    (void)controller; (void)x; (void)y;
    GhosttyTerminal *self = GHOSTTY_TERMINAL(user_data);
    if (self->gl_area)
        gtk_widget_grab_focus(GTK_WIDGET(self->gl_area));
}

/* ── Scroll ────────────────────────────────────────────────────── */

static gboolean
on_scroll(GtkEventControllerScroll *controller,
          double                    dx,
          double                    dy,
          gpointer                  user_data)
{
    GhosttyTerminal *self = GHOSTTY_TERMINAL(user_data);

    if (!self->surface)
        return FALSE;

    GdkModifierType state = gtk_event_controller_get_current_event_state(
        GTK_EVENT_CONTROLLER(controller));
    ghostty_surface_mouse_scroll(self->surface, dx, dy,
                                 (ghostty_input_scroll_mods_t)translate_mods(state));
    gtk_gl_area_queue_render(self->gl_area);
    return TRUE;
}

/* ── Focus ─────────────────────────────────────────────────────── */

static void
on_focus_enter(GtkEventControllerFocus *controller, gpointer user_data)
{
    (void)controller;
    GhosttyTerminal *self = GHOSTTY_TERMINAL(user_data);

    if (self->surface)
        ghostty_surface_set_focus(self->surface, true);

    gtk_im_context_focus_in(self->im_context);
}

static void
on_focus_leave(GtkEventControllerFocus *controller, gpointer user_data)
{
    (void)controller;
    GhosttyTerminal *self = GHOSTTY_TERMINAL(user_data);

    if (self->surface)
        ghostty_surface_set_focus(self->surface, false);

    gtk_im_context_focus_out(self->im_context);
}

/* ── GObject lifecycle ─────────────────────────────────────────── */

static void
ghostty_terminal_dispose(GObject *object)
{
    GhosttyTerminal *self = GHOSTTY_TERMINAL(object);

    if (self->tick_source_id) {
        g_source_remove(self->tick_source_id);
        self->tick_source_id = 0;
    }

    if (self->surface) {
        ghostty_surface_free(self->surface);
        self->surface = NULL;
    }

    g_clear_object(&self->im_context);

    /* Remove the child GtkGLArea from the widget tree */
    if (self->gl_area) {
        gtk_widget_unparent(GTK_WIDGET(self->gl_area));
        self->gl_area = NULL;
    }

    G_OBJECT_CLASS(ghostty_terminal_parent_class)->dispose(object);
}

static void
ghostty_terminal_finalize(GObject *object)
{
    GhosttyTerminal *self = GHOSTTY_TERMINAL(object);

    g_free(self->title);
    g_free(self->cwd);
    g_free(self->start_cwd);

    G_OBJECT_CLASS(ghostty_terminal_parent_class)->finalize(object);
}

/* ── Class init ────────────────────────────────────────────────── */

static void
ghostty_terminal_class_init(GhosttyTerminalClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->dispose = ghostty_terminal_dispose;
    object_class->finalize = ghostty_terminal_finalize;

    /* Layout: the GtkGLArea fills the entire widget */
    gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);

    /* Signals */
    signals[SIGNAL_TITLE_CHANGED] = g_signal_new(
        "title-changed",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[SIGNAL_PWD_CHANGED] = g_signal_new(
        "pwd-changed",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[SIGNAL_COMMAND_FINISHED] = g_signal_new(
        "command-finished",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 2, G_TYPE_INT, G_TYPE_UINT64);

    signals[SIGNAL_BELL] = g_signal_new(
        "bell",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 0);

    signals[SIGNAL_PROCESS_EXITED] = g_signal_new(
        "process-exited",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_INT);

    signals[SIGNAL_CLOSE_REQUESTED] = g_signal_new(
        "close-requested",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 0);
}

static void
ghostty_terminal_init(GhosttyTerminal *self)
{
    self->surface = NULL;
    self->tick_source_id = 0;
    self->exit_emitted = FALSE;
    self->title = NULL;
    self->cwd = NULL;
    self->start_cwd = NULL;
    self->exit_code = -1;
    self->has_new_output = FALSE;
    self->progress_state = -1;
    self->progress_percent = -1;


    /* ── Create the GtkGLArea child ── */

    self->gl_area = GTK_GL_AREA(gtk_gl_area_new());
    gtk_gl_area_set_auto_render(self->gl_area, FALSE);
    gtk_gl_area_set_use_es(self->gl_area, FALSE);
    gtk_gl_area_set_required_version(self->gl_area, 4, 3);
    gtk_widget_set_hexpand(GTK_WIDGET(self->gl_area), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(self->gl_area), TRUE);
    gtk_widget_set_focusable(GTK_WIDGET(self->gl_area), TRUE);
    gtk_widget_set_parent(GTK_WIDGET(self->gl_area), GTK_WIDGET(self));

    g_signal_connect(self->gl_area, "realize", G_CALLBACK(on_gl_realize), self);
    g_signal_connect(self->gl_area, "render", G_CALLBACK(on_gl_render), self);
    g_signal_connect(self->gl_area, "resize", G_CALLBACK(on_gl_resize), self);

    /* ── IME context ── */

    self->im_context = gtk_im_multicontext_new();
    g_signal_connect(self->im_context, "commit",
                     G_CALLBACK(on_im_commit), self);
    g_signal_connect(self->im_context, "preedit-changed",
                     G_CALLBACK(on_im_preedit_changed), self);

    /* ── Keyboard controller (capture phase for shortcuts) ── */

    GtkEventController *key_ctrl = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(key_ctrl, GTK_PHASE_CAPTURE);
    g_signal_connect(key_ctrl, "key-pressed",
                     G_CALLBACK(on_key_pressed), self);
    g_signal_connect(key_ctrl, "key-released",
                     G_CALLBACK(on_key_released), self);
    gtk_widget_add_controller(GTK_WIDGET(self->gl_area), key_ctrl);

    /* Set the IME context's client widget */
    gtk_im_context_set_client_widget(self->im_context,
                                     GTK_WIDGET(self->gl_area));

    /* ── Mouse click (all three buttons) ── */

    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0); /* all buttons */
    g_signal_connect(click, "pressed",
                     G_CALLBACK(on_click_pressed), self);
    g_signal_connect(click, "released",
                     G_CALLBACK(on_click_released), self);
    gtk_widget_add_controller(GTK_WIDGET(self->gl_area),
                              GTK_EVENT_CONTROLLER(click));

    /* ── Mouse motion ── */

    GtkEventController *motion_ctrl = gtk_event_controller_motion_new();
    g_signal_connect(motion_ctrl, "motion",
                     G_CALLBACK(on_motion), self);
    g_signal_connect(motion_ctrl, "enter",
                     G_CALLBACK(on_mouse_enter), self);
    gtk_widget_add_controller(GTK_WIDGET(self->gl_area), motion_ctrl);

    /* ── Scroll ── */

    GtkEventController *scroll_ctrl = gtk_event_controller_scroll_new(
        GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    g_signal_connect(scroll_ctrl, "scroll",
                     G_CALLBACK(on_scroll), self);
    gtk_widget_add_controller(GTK_WIDGET(self->gl_area), scroll_ctrl);

    /* ── Focus ── */

    GtkEventController *focus_ctrl = gtk_event_controller_focus_new();
    g_signal_connect(focus_ctrl, "enter",
                     G_CALLBACK(on_focus_enter), self);
    g_signal_connect(focus_ctrl, "leave",
                     G_CALLBACK(on_focus_leave), self);
    gtk_widget_add_controller(GTK_WIDGET(self->gl_area), focus_ctrl);

    /* ── Tick timer (16 ms ~ 60 Hz) ── */

    self->tick_source_id = g_timeout_add(16, tick_callback, self);
}

/* ── Public API ────────────────────────────────────────────────── */

GtkWidget *
ghostty_terminal_new(const char *start_cwd)
{
    GhosttyTerminal *self = g_object_new(GHOSTTY_TYPE_TERMINAL, NULL);
    if (start_cwd && *start_cwd)
        self->start_cwd = g_strdup(start_cwd);
    return GTK_WIDGET(self);
}

ghostty_surface_t
ghostty_terminal_get_surface(GhosttyTerminal *self)
{
    g_return_val_if_fail(GHOSTTY_IS_TERMINAL(self), NULL);
    return self->surface;
}

const char *
ghostty_terminal_get_title(GhosttyTerminal *self)
{
    g_return_val_if_fail(GHOSTTY_IS_TERMINAL(self), NULL);
    return self->title;
}

const char *
ghostty_terminal_get_cwd(GhosttyTerminal *self)
{
    g_return_val_if_fail(GHOSTTY_IS_TERMINAL(self), NULL);
    return self->cwd;
}

int
ghostty_terminal_get_exit_code(GhosttyTerminal *self)
{
    g_return_val_if_fail(GHOSTTY_IS_TERMINAL(self), -1);
    return self->exit_code;
}

void
ghostty_terminal_set_title(GhosttyTerminal *self, const char *title)
{
    g_return_if_fail(GHOSTTY_IS_TERMINAL(self));
    g_free(self->title);
    self->title = g_strdup(title);
    g_signal_emit(self, signals[SIGNAL_TITLE_CHANGED], 0, self->title);
}

void
ghostty_terminal_set_cwd(GhosttyTerminal *self, const char *cwd)
{
    g_return_if_fail(GHOSTTY_IS_TERMINAL(self));
    g_free(self->cwd);
    self->cwd = g_strdup(cwd);
    g_signal_emit(self, signals[SIGNAL_PWD_CHANGED], 0, self->cwd);
}

void
ghostty_terminal_notify_bell(GhosttyTerminal *self)
{
    g_return_if_fail(GHOSTTY_IS_TERMINAL(self));
    g_signal_emit(self, signals[SIGNAL_BELL], 0);
}

void
ghostty_terminal_notify_command_finished(GhosttyTerminal *self,
                                         int              exit_code,
                                         uint64_t         duration_ns)
{
    g_return_if_fail(GHOSTTY_IS_TERMINAL(self));
    g_signal_emit(self, signals[SIGNAL_COMMAND_FINISHED], 0,
                  exit_code, (guint64)duration_ns);
}

void
ghostty_terminal_notify_child_exited(GhosttyTerminal *self,
                                     uint32_t         exit_code)
{
    g_return_if_fail(GHOSTTY_IS_TERMINAL(self));
    self->exit_code = (int)exit_code;
    if (!self->exit_emitted) {
        self->exit_emitted = TRUE;
        g_signal_emit(self, signals[SIGNAL_PROCESS_EXITED], 0, (int)exit_code);
    }
}

void
ghostty_terminal_queue_render(GhosttyTerminal *self)
{
    g_return_if_fail(GHOSTTY_IS_TERMINAL(self));
    if (self->gl_area)
        gtk_gl_area_queue_render(self->gl_area);
}

/* ── Activity tracking ────────────────────────────────────────── */

void
ghostty_terminal_mark_activity(GhosttyTerminal *self)
{
    g_return_if_fail(GHOSTTY_IS_TERMINAL(self));
    self->has_new_output = TRUE;
}

void
ghostty_terminal_clear_activity(GhosttyTerminal *self)
{
    g_return_if_fail(GHOSTTY_IS_TERMINAL(self));
    self->has_new_output = FALSE;
}

gboolean
ghostty_terminal_has_activity(GhosttyTerminal *self)
{
    g_return_val_if_fail(GHOSTTY_IS_TERMINAL(self), FALSE);
    return self->has_new_output;
}

/* ── Progress bar ─────────────────────────────────────────────── */

void
ghostty_terminal_set_progress(GhosttyTerminal *self, int state, int percent)
{
    g_return_if_fail(GHOSTTY_IS_TERMINAL(self));
    self->progress_state = state;
    self->progress_percent = percent;
}

int
ghostty_terminal_get_progress_percent(GhosttyTerminal *self)
{
    g_return_val_if_fail(GHOSTTY_IS_TERMINAL(self), -1);
    return self->progress_percent;
}

int
ghostty_terminal_get_progress_state(GhosttyTerminal *self)
{
    g_return_val_if_fail(GHOSTTY_IS_TERMINAL(self), -1);
    return self->progress_state;
}
