/*
 * port_scanner.c - Periodic port scanning for dev servers
 *
 * Every 5 seconds, reads /proc/net/tcp and /proc/net/tcp6 to find
 * listening sockets. Detects newly appeared ports and fires a callback.
 */

#include "port_scanner.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── State ────────────────────────────────────────────────────────── */

static guint              scan_timer_id = 0;
static GArray            *last_known_ports = NULL; /* int[] */
static PortScannerCallback scan_callback = NULL;
static gpointer           scan_user_data = NULL;

/* ── Helpers ──────────────────────────────────────────────────────── */

static gboolean
is_system_service_port(int port)
{
    /* Postgres */
    if (port == 5432 || port == 5433 || port == 5434)
        return TRUE;
    /* Redis */
    if (port == 6379 || port == 6380)
        return TRUE;
    /* Ollama */
    if (port == 11434)
        return TRUE;
    /* ADB / Avahi */
    if (port == 4723 || port == 5037 || port == 5054)
        return TRUE;
    /* Ephemeral range */
    if (port >= 35000)
        return TRUE;
    return FALSE;
}

static int
int_compare(gconstpointer a, gconstpointer b)
{
    int ia = *(const int *)a;
    int ib = *(const int *)b;
    return (ia > ib) - (ia < ib);
}

static gboolean
int_array_contains(GArray *arr, int val)
{
    for (guint i = 0; i < arr->len; i++) {
        if (g_array_index(arr, int, i) == val)
            return TRUE;
    }
    return FALSE;
}

static void
scan_file(const char *path, GArray *ports)
{
    FILE *fp = fopen(path, "r");
    if (!fp)
        return;

    char line[512];
    /* Skip header line */
    if (fgets(line, sizeof(line), fp) == NULL) {
        fclose(fp);
        return;
    }

    while (fgets(line, sizeof(line), fp)) {
        /*
         * /proc/net/tcp format (space-separated, with leading spaces):
         *   sl  local_address  rem_address  st  ...
         * Fields split by whitespace. We need field 1 (local_addr:port)
         * and field 3 (state). State "0A" = LISTEN.
         */
        char *fields[8];
        int nfields = 0;
        char *saveptr = NULL;
        char *tok = strtok_r(line, " \t\n", &saveptr);
        while (tok && nfields < 8) {
            fields[nfields++] = tok;
            tok = strtok_r(NULL, " \t\n", &saveptr);
        }

        if (nfields < 4)
            continue;

        /* Field 3 is the state */
        if (strcmp(fields[3], "0A") != 0)
            continue;

        /* Field 1 is local_address in HEX_IP:HEX_PORT */
        char *colon = strrchr(fields[1], ':');
        if (!colon)
            continue;

        unsigned int port = 0;
        if (sscanf(colon + 1, "%x", &port) != 1 || port == 0)
            continue;

        /* Filter: only dev ports 1024-34999, skip system services */
        if (port < 1024)
            continue;
        if (is_system_service_port((int)port))
            continue;

        int p = (int)port;
        if (!int_array_contains(ports, p))
            g_array_append_val(ports, p);
    }

    fclose(fp);
}

/* ── Timer callback ───────────────────────────────────────────────── */

static gboolean
scan_ports_tick(gpointer data)
{
    (void)data;

    GArray *ports = g_array_new(FALSE, FALSE, sizeof(int));

    scan_file("/proc/net/tcp", ports);
    scan_file("/proc/net/tcp6", ports);

    /* Sort */
    g_array_sort(ports, int_compare);

    /* Detect new ports */
    GArray *new_ports = g_array_new(FALSE, FALSE, sizeof(int));
    for (guint i = 0; i < ports->len; i++) {
        int p = g_array_index(ports, int, i);
        if (!last_known_ports || !int_array_contains(last_known_ports, p))
            g_array_append_val(new_ports, p);
    }

    /* Update last known */
    if (last_known_ports)
        g_array_unref(last_known_ports);
    last_known_ports = g_array_ref(ports);

    /* Fire callback if there are new ports */
    if (new_ports->len > 0 && scan_callback) {
        scan_callback((const int *)new_ports->data,
                      (int)new_ports->len,
                      (const int *)ports->data,
                      (int)ports->len,
                      scan_user_data);
    }

    g_array_unref(new_ports);
    g_array_unref(ports);

    return G_SOURCE_CONTINUE;
}

/* ── Public API ───────────────────────────────────────────────────── */

void
port_scanner_set_callback(PortScannerCallback cb, gpointer user_data)
{
    scan_callback = cb;
    scan_user_data = user_data;
}

void
port_scanner_start(void)
{
    if (scan_timer_id != 0)
        return;

    last_known_ports = g_array_new(FALSE, FALSE, sizeof(int));

    /* Do an initial scan immediately so we have a baseline */
    scan_ports_tick(NULL);

    /* Then every 5 seconds */
    scan_timer_id = g_timeout_add_seconds(5, scan_ports_tick, NULL);
}

void
port_scanner_stop(void)
{
    if (scan_timer_id != 0) {
        g_source_remove(scan_timer_id);
        scan_timer_id = 0;
    }

    if (last_known_ports) {
        g_array_unref(last_known_ports);
        last_known_ports = NULL;
    }
}
