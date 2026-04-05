/*
 * port_scanner.h - Centralized listening-port tracking for PrettyMux
 *
 * A single app-owned monitor tracks listening TCP sockets on Linux and
 * attributes them to registered PrettyMux terminals. Terminals are
 * registered once by the shell integration; the scanner itself runs only
 * in the app process.
 */
#pragma once

#include <glib.h>
#include <sys/types.h>

G_BEGIN_DECLS

typedef void (*PortScannerCallback)(const char *terminal_id,
                                    int         port,
                                    gpointer    user_data);

void port_scanner_start(void);
void port_scanner_stop(void);
void port_scanner_set_callback(PortScannerCallback cb, gpointer user_data);
void port_scanner_register_terminal(const char *terminal_id,
                                    pid_t       session_id,
                                    const char *tty_name,
                                    const char *tty_path,
                                    int         workspace_idx);
void port_scanner_unregister_terminal(const char *terminal_id);
void port_scanner_set_terminal_workspace(const char *terminal_id,
                                         int         workspace_idx);
void port_scanner_set_active_workspace(int workspace_idx);
void port_scanner_set_window_active(gboolean active);

G_END_DECLS
