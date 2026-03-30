#ifdef _WIN32

#include <BaseTsd.h>
typedef SSIZE_T ssize_t;

#include "ghostty.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
  ghostty_surface_size_s size;
  bool exited;
} ghostty_stub_surface_s;

int ghostty_init(uintptr_t argc, char** argv) {
  (void)argc;
  (void)argv;
  return GHOSTTY_SUCCESS;
}

ghostty_config_t ghostty_config_new() {
  return malloc(1);
}

void ghostty_config_load_default_files(ghostty_config_t config) {
  (void)config;
}

void ghostty_config_finalize(ghostty_config_t config) {
  (void)config;
}

ghostty_app_t ghostty_app_new(const ghostty_runtime_config_s* runtime_config,
                              ghostty_config_t config) {
  (void)runtime_config;
  (void)config;
  return malloc(1);
}

void ghostty_app_free(ghostty_app_t app) {
  free(app);
}

void ghostty_app_tick(ghostty_app_t app) {
  (void)app;
}

ghostty_surface_config_s ghostty_surface_config_new() {
  ghostty_surface_config_s config;
  memset(&config, 0, sizeof(config));
  return config;
}

ghostty_surface_t ghostty_surface_new(ghostty_app_t app,
                                      const ghostty_surface_config_s* config) {
  (void)app;
  (void)config;
  return calloc(1, sizeof(ghostty_stub_surface_s));
}

void ghostty_surface_free(ghostty_surface_t surface) {
  free(surface);
}

bool ghostty_surface_process_exited(ghostty_surface_t surface) {
  ghostty_stub_surface_s* stub = (ghostty_stub_surface_s*)surface;
  return stub ? stub->exited : false;
}

void ghostty_surface_init_opengl(ghostty_surface_t surface) {
  (void)surface;
}

void ghostty_surface_set_focus(ghostty_surface_t surface, bool focused) {
  (void)surface;
  (void)focused;
}

void ghostty_surface_set_content_scale(ghostty_surface_t surface,
                                       double x,
                                       double y) {
  (void)surface;
  (void)x;
  (void)y;
}

void ghostty_surface_set_size(ghostty_surface_t surface,
                              uint32_t width,
                              uint32_t height) {
  ghostty_stub_surface_s* stub = (ghostty_stub_surface_s*)surface;
  if (!stub) return;
  stub->size.width_px = width;
  stub->size.height_px = height;
}

void ghostty_surface_draw_frame(ghostty_surface_t surface) {
  (void)surface;
}

bool ghostty_surface_has_selection(ghostty_surface_t surface) {
  (void)surface;
  return false;
}

bool ghostty_surface_read_selection(ghostty_surface_t surface,
                                    ghostty_text_s* text) {
  (void)surface;
  if (text) memset(text, 0, sizeof(*text));
  return false;
}

void ghostty_surface_free_text(ghostty_surface_t surface, ghostty_text_s* text) {
  (void)surface;
  if (text) memset(text, 0, sizeof(*text));
}

void ghostty_surface_text(ghostty_surface_t surface,
                          const char* text,
                          uintptr_t len) {
  (void)surface;
  (void)text;
  (void)len;
}

bool ghostty_surface_key(ghostty_surface_t surface, ghostty_input_key_s key) {
  (void)surface;
  (void)key;
  return false;
}

bool ghostty_surface_mouse_button(ghostty_surface_t surface,
                                  ghostty_input_mouse_state_e state,
                                  ghostty_input_mouse_button_e button,
                                  ghostty_input_mods_e mods) {
  (void)surface;
  (void)state;
  (void)button;
  (void)mods;
  return false;
}

void ghostty_surface_mouse_pos(ghostty_surface_t surface,
                               double x,
                               double y,
                               ghostty_input_mods_e mods) {
  (void)surface;
  (void)x;
  (void)y;
  (void)mods;
}

void ghostty_surface_mouse_scroll(ghostty_surface_t surface,
                                  double x,
                                  double y,
                                  ghostty_input_scroll_mods_t mods) {
  (void)surface;
  (void)x;
  (void)y;
  (void)mods;
}

bool ghostty_surface_binding_action(ghostty_surface_t surface,
                                    const char* action,
                                    uintptr_t len) {
  (void)surface;
  (void)action;
  (void)len;
  return false;
}

ghostty_surface_size_s ghostty_surface_size(ghostty_surface_t surface) {
  ghostty_stub_surface_s* stub = (ghostty_stub_surface_s*)surface;
  ghostty_surface_size_s size;
  memset(&size, 0, sizeof(size));
  if (stub) size = stub->size;
  return size;
}

bool ghostty_surface_read_text(ghostty_surface_t surface,
                               ghostty_selection_s selection,
                               ghostty_text_s* text) {
  (void)surface;
  (void)selection;
  if (text) memset(text, 0, sizeof(*text));
  return false;
}

#endif
