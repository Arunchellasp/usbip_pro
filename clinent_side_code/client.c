/*
 * client.c  –  USB/IP TCP Client for Windows
 *
 * Build (MSVC Developer Command Prompt):
 *   cl client.c /W4 /O2 /Fe:usbip_client.exe Ws2_32.lib
 *
 * Usage:
 *   usbip_client.exe <server_ip> [port]    (default port: 5000)
 */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#pragma comment(lib, "Ws2_32.lib")

/* ── Configuration ─────────────────────────────────────────────── */
#define DEFAULT_PORT   5000
#define CMD_BUF_SIZE   256
#define RECV_BUF_SIZE  4096
#define OUT_BUF_SIZE   65536
#define SENTINEL       "END_OF_RESPONSE\n"
#define SENTINEL_LEN   16          /* strlen(SENTINEL) */
#define MAX_BUSIDS     64
#define BUSID_LEN      32

/* ── Globals ────────────────────────────────────────────────────── */
static char g_server_ip[64];
static int  g_port;

/* ── Utilities ──────────────────────────────────────────────────── */
static void rtrim(char *s)
{
    int i = (int)strlen(s) - 1;
    while (i >= 0 && (s[i] == '\n' || s[i] == '\r' || s[i] == ' '))
        s[i--] = '\0';
}

static int send_all(SOCKET sock, const char *buf, int len)
{
    int sent = 0;
    while (sent < len) {
        int n = send(sock, buf + sent, len - sent, 0);
        if (n == SOCKET_ERROR) return -1;
        sent += n;
    }
    return 0;
}

/*
 * Receive bytes from the socket and print them to stdout, stopping
 * when the sentinel "END_OF_RESPONSE\n" is detected.
 * Returns 0 on success, -1 on socket error / unexpected close.
 */
static int recv_response(SOCKET sock)
{
    char buf[RECV_BUF_SIZE + 1];

    /*
     * We keep the last (SENTINEL_LEN - 1) bytes of previously received
     * data in `tail` so the sentinel is detected even if it straddles
     * two recv() calls.
     */
    char tail[SENTINEL_LEN + 1];
    int  tail_len = 0;
    memset(tail, 0, sizeof(tail));

    while (1) {
        int n = recv(sock, buf, RECV_BUF_SIZE, 0);
        if (n == SOCKET_ERROR) {
            fprintf(stderr, "[ERROR] recv failed: %d\n", WSAGetLastError());
            return -1;
        }
        if (n == 0) {
            fprintf(stderr, "[ERROR] Server closed the connection unexpectedly.\n");
            return -1;
        }
        buf[n] = '\0';

        /* Build a search window: tail + new data */
        char window[SENTINEL_LEN + RECV_BUF_SIZE + 1];
        memcpy(window, tail, (size_t)tail_len);
        memcpy(window + tail_len, buf, (size_t)(n + 1));
        int wlen = tail_len + n;

        char *end = strstr(window, SENTINEL);
        if (end) {
            /* Print everything up to the sentinel (skip the tail overlap) */
            *end = '\0';
            char *print_start = window + tail_len;
            if (end > print_start)
                fwrite(print_start, 1, (size_t)(end - print_start), stdout);
            fflush(stdout);
            return 0;
        }

        /* Print the safe portion (exclude last SENTINEL_LEN-1 bytes) */
        int safe = wlen - (SENTINEL_LEN - 1);
        if (safe < 0) safe = 0;
        if (safe > tail_len)
            fwrite(window + tail_len, 1, (size_t)(safe - tail_len), stdout);
        fflush(stdout);

        /* Update tail */
        int new_tail = (SENTINEL_LEN - 1 < wlen) ? SENTINEL_LEN - 1 : wlen;
        tail_len = new_tail;
        memcpy(tail, window + wlen - tail_len, (size_t)tail_len);
        tail[tail_len] = '\0';
    }
}

/* ── Local command helpers ──────────────────────────────────────── */

/* Run a local command with _popen() and print its output. */
static int local_run(const char *cmd)
{
    printf(">>> %s\n", cmd);
    fflush(stdout);

    char full[512];
    snprintf(full, sizeof(full), "%s 2>&1", cmd);

    FILE *fp = _popen(full, "r");
    if (!fp) {
        fprintf(stderr,
                "[ERROR] Failed to execute: %s\n"
                "        Make sure usbip.exe is on PATH.\n", cmd);
        return -1;
    }

    char   line[512];
    int    got_output = 0;
    while (fgets(line, sizeof(line), fp)) {
        fputs(line, stdout);
        got_output = 1;
    }
    if (!got_output) printf("(no output)\n");

    int rc = _pclose(fp);
    printf("(exit code: %d)\n", rc);
    fflush(stdout);
    return rc;
}

/*
 * Parse the output of `usbip list -r` and extract bus IDs.
 * Returns the number of bus IDs found.
 *
 * Relevant lines start with leading whitespace followed by a digit and
 * contain a '-' (e.g. "   1-1: ...").
 */
static int parse_remote_busids(const char *output,
                               char busids[][BUSID_LEN], int max)
{
    int count = 0;
    const char *p = output;

    while (*p && count < max) {
        /* Skip to first non-space on this line */
        while (*p == ' ' || *p == '\t') p++;

        if (isdigit((unsigned char)*p)) {
            char token[BUSID_LEN];
            int  tlen = 0;
            while (*p && *p != ':' && *p != ' ' && tlen < BUSID_LEN - 1)
                token[tlen++] = *p++;
            token[tlen] = '\0';

            /* A valid bus ID contains '-' */
            if (tlen > 0 && strchr(token, '-')) {
                strncpy(busids[count], token, BUSID_LEN - 1);
                busids[count][BUSID_LEN - 1] = '\0';
                count++;
            }
        }

        /* Advance to next line */
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }

    return count;
}

/* ── Local commands ─────────────────────────────────────────────── */

static void cmd_local_list(void)
{
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "usbip list -r %s", g_server_ip);
    local_run(cmd);
}

static void cmd_local_bind_busid(const char *busid)
{
    for (const char *p = busid; *p; p++) {
        if (!isdigit((unsigned char)*p) && *p != '-' && *p != '.') {
            fprintf(stderr, "[ERROR] Invalid bus ID: '%s'\n", busid);
            return;
        }
    }
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "usbip attach -r %s -b %s",
             g_server_ip, busid);
    local_run(cmd);
}

static void cmd_local_bind_all(void)
{
    char list_cmd[128];
    snprintf(list_cmd, sizeof(list_cmd), "usbip list -r %s 2>&1", g_server_ip);
    printf(">>> %s\n", list_cmd);
    fflush(stdout);

    FILE *fp = _popen(list_cmd, "r");
    if (!fp) {
        fprintf(stderr,
                "[ERROR] Failed to run: %s\n"
                "        Make sure usbip.exe is on PATH.\n", list_cmd);
        return;
    }

    char   output[OUT_BUF_SIZE];
    size_t total = 0;
    char   line[512];
    while (fgets(line, sizeof(line), fp)) {
        fputs(line, stdout);
        size_t len = strlen(line);
        if (total + len < OUT_BUF_SIZE - 1) {
            memcpy(output + total, line, len);
            total += len;
        }
    }
    output[total] = '\0';
    _pclose(fp);
    fflush(stdout);

    char busids[MAX_BUSIDS][BUSID_LEN];
    int  count = parse_remote_busids(output, busids, MAX_BUSIDS);

    if (count == 0) {
        printf("[local_bind_all] No remote USB devices found.\n");
        return;
    }

    printf("\n[local_bind_all] Found %d device(s). Attaching...\n\n", count);
    for (int i = 0; i < count; i++) {
        char attach_cmd[256];
        snprintf(attach_cmd, sizeof(attach_cmd),
                 "usbip attach -r %s -b %s", g_server_ip, busids[i]);
        local_run(attach_cmd);
        printf("\n");
    }
}

/*
 * Detach every currently attached USB/IP port on this Windows machine.
 * Uses:  usbip detach --all
 */
/*
 * Detach every currently attached USB/IP port on this Windows machine.
 *
 * "usbip detach --all" is NOT valid on the Windows usbip build.
 * Instead we run "usbip port", parse every "Port NN:" line,
 * and call "usbip detach -p NN" for each one.
 *
 * usbip port output looks like:
 *   Port 00: <status> at Full Speed(12Mbps)
 *       ...
 *   Port 01: <status> ...
 */
static void cmd_local_detach_all(void)
{
    printf("[detach] Checking for attached USB/IP ports...\n");
    fflush(stdout);

    FILE *fp = _popen("usbip port 2>&1", "r");
    if (!fp) {
        fprintf(stderr, "[detach] WARNING: could not run 'usbip port'\n");
        return;
    }

    char   output[OUT_BUF_SIZE];
    size_t total = 0;
    char   line[512];
    while (fgets(line, sizeof(line), fp) &&
           total + strlen(line) < OUT_BUF_SIZE - 1) {
        fputs(line, stdout);
        size_t len = strlen(line);
        memcpy(output + total, line, len);
        total += len;
    }
    output[total] = '\0';
    _pclose(fp);
    fflush(stdout);

    /* Parse "Port NN:" lines and detach each one */
    int detached = 0;
    const char *p = output;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;  /* skip leading whitespace */

        if (strncmp(p, "Port ", 5) == 0) {
            p += 5;
            /* Read port number (up to 3 digits) */
            char numstr[8];
            int  nlen = 0;
            while (*p && isdigit((unsigned char)*p) && nlen < 7)
                numstr[nlen++] = *p++;
            numstr[nlen] = '\0';

            /* Must be followed by ':' to be a real port line */
            if (nlen > 0 && *p == ':') {
                char detach_cmd[64];
                snprintf(detach_cmd, sizeof(detach_cmd),
                         "usbip detach -p %s", numstr);
                printf("\n");
                local_run(detach_cmd);
                detached++;
            }
        }

        while (*p && *p != '\n') p++;  /* advance to next line */
        if (*p == '\n') p++;
    }

    if (detached == 0)
        printf("[detach] No attached USB/IP ports found.\n");
    else
        printf("\n[detach] Detached %d port(s).\n", detached);
    fflush(stdout);
}

/*
 * pro_bind  –  full one-command sequence:
 *   1. [local]  detach all already-attached USB/IP ports
 *   2. [remote] send "pro_bind" to server
 *               (server runs: usbserver_init -> list_usb -> bind_all)
 *   3. [local]  list remote devices   (usbip list -r <ip>)
 *   4. [local]  attach all remote devices to Windows
 */
static void cmd_pro_bind(SOCKET sock)
{
    printf("\n");
    printf("  +----------------------------------------------------------+\n");
    printf("  |  pro_bind  :  full USB/IP setup sequence                 |\n");
    printf("  |  Step 1/4  :  detach existing local ports               |\n");
    printf("  |  Step 2/4  :  server init + list + bind (remote)        |\n");
    printf("  |  Step 3/4  :  list remote devices (local)               |\n");
    printf("  |  Step 4/4  :  attach all remote devices (local)         |\n");
    printf("  +----------------------------------------------------------+\n\n");

    /* Step 1: detach already-attached ports */
    printf("--- [1/4] Detach existing local ports ---\n");
    cmd_local_detach_all();
    printf("---\n\n");

    /* Step 2: server-side init + list + bind */
    printf("--- [2/4] Server: usbserver_init + list_usb + bind_all ---\n");
    const char *remote_cmd = "pro_bind\n";
    if (send_all(sock, remote_cmd, (int)strlen(remote_cmd)) != 0) {
        fprintf(stderr, "[ERROR] send() failed: %d\n"
                "        Connection to server may have been lost.\n",
                WSAGetLastError());
        return;
    }
    if (recv_response(sock) != 0) return;
    printf("---\n\n");

    /* Step 3: local list */
    printf("--- [3/4] List remote devices ---\n");
    cmd_local_list();
    printf("---\n\n");

    /* Step 4: local attach all */
    printf("--- [4/4] Attach all remote devices to Windows ---\n");
    cmd_local_bind_all();
    printf("---\n\n");

    printf("[pro_bind] Done.\n\n");
}

/* ── Help text ──────────────────────────────────────────────────── */
static void print_help(void)
{
    printf("\n");
    printf("  +------------------------------------------------------------------+\n");
    printf("  |               USB/IP Client Command Reference                    |\n");
    printf("  +------------------+------------------------------------------------+\n");
    printf("  | REMOTE COMMANDS  | sent to server at %-24s |\n", g_server_ip);
    printf("  +------------------+------------------------------------------------+\n");
    printf("  |  usbserver_init     load kernel modules + start usbipd           |\n");
    printf("  |  list_usb           list USB devices on the Pi                   |\n");
    printf("  |  bind_all           bind all Pi USB devices for export            |\n");
    printf("  |  bind_list          show currently bound devices on Pi            |\n");
    printf("  |  bind_<busid>       bind specific device  e.g. bind_1-1          |\n");
    printf("  +------------------------------------------------------------------+\n");
    printf("  | LOCAL COMMANDS   (run on THIS Windows machine)                   |\n");
    printf("  +------------------------------------------------------------------+\n");
    printf("  |  local_list         usbip list -r <server_ip>                    |\n");
    printf("  |  local_bind_all     attach all remote devices to Windows          |\n");
    printf("  |  local_bind_<id>    usbip attach -r <ip> -b <busid>              |\n");
    printf("  |                     e.g. local_bind_1-1.1                        |\n");
    printf("  +------------------------------------------------------------------+\n");
    printf("  |  help               show this help                               |\n");
    printf("  |  exit / quit        disconnect and close                         |\n");
    printf("  +------------------------------------------------------------------+\n");
    printf("  | ONE-COMMAND SETUP                                                 |\n");
    printf("  +------------------------------------------------------------------+\n");
    printf("  |  pro_bind           detach local ports, then server-side         |\n");
    printf("  |                     init+list+bind, then local list+attach       |\n");
    printf("  +------------------------------------------------------------------+\n");
    printf("\n");
}

/* ── main ───────────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <server_ip> [port]  (default port: %d)\n",
                argv[0], DEFAULT_PORT);
        return 1;
    }

    strncpy(g_server_ip, argv[1], sizeof(g_server_ip) - 1);
    g_server_ip[sizeof(g_server_ip) - 1] = '\0';

    g_port = (argc >= 3) ? atoi(argv[2]) : DEFAULT_PORT;
    if (g_port <= 0 || g_port > 65535) {
        fprintf(stderr, "Invalid port: %s\n", argv[2]);
        return 1;
    }

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed: %d\n", WSAGetLastError());
        return 1;
    }

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        fprintf(stderr, "socket() failed: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port   = htons((u_short)g_port);

    if (inet_pton(AF_INET, g_server_ip, &server_addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid server IP address: %s\n", g_server_ip);
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    printf("[client] Connecting to %s:%d ...\n", g_server_ip, g_port);
    if (connect(sock, (struct sockaddr *)&server_addr,
                sizeof(server_addr)) == SOCKET_ERROR) {
        fprintf(stderr,
                "[ERROR] connect() failed: %d\n"
                "        Check the server is running and the IP/port are correct.\n",
                WSAGetLastError());
        closesocket(sock);
        WSACleanup();
        return 1;
    }
    printf("[client] Connected to %s:%d\n", g_server_ip, g_port);
    print_help();

    char cmd[CMD_BUF_SIZE];
    while (1) {
        printf("usbip> ");
        fflush(stdout);

        if (!fgets(cmd, sizeof(cmd), stdin)) break;
        rtrim(cmd);
        if (cmd[0] == '\0') continue;

        /* ── Local / housekeeping commands (no socket needed) ── */
        if (_stricmp(cmd, "exit") == 0 || _stricmp(cmd, "quit") == 0) {
            printf("[client] Disconnecting.\n");
            break;
        }
        if (_stricmp(cmd, "help") == 0) {
            print_help();
            continue;
        }
        if (_stricmp(cmd, "local_list") == 0) {
            printf("---\n"); cmd_local_list(); printf("---\n\n");
            continue;
        }
        if (_stricmp(cmd, "local_bind_all") == 0) {
            printf("---\n"); cmd_local_bind_all(); printf("---\n\n");
            continue;
        }
        if (_strnicmp(cmd, "local_bind_", 11) == 0) {
            const char *busid = cmd + 11;
            if (*busid == '\0')
                fprintf(stderr,
                        "[ERROR] Missing bus ID. Usage: local_bind_<busid>"
                        "  e.g. local_bind_1-1.1\n");
            else {
                printf("---\n"); cmd_local_bind_busid(busid); printf("---\n\n");
            }
            continue;
        }
        if (_stricmp(cmd, "pro_bind") == 0) {
            cmd_pro_bind(sock);
            continue;
        }
        if (_strnicmp(cmd, "local", 5) == 0) {
            fprintf(stderr,
                    "[ERROR] Unknown local command '%s'.\n"
                    "        Local commands: local_list, local_bind_all,"
                    " local_bind_<busid>\n", cmd);
            continue;
        }

        /* ── Remote command: send to server ── */
        char to_send[CMD_BUF_SIZE + 2];
        snprintf(to_send, sizeof(to_send), "%s\n", cmd);
        if (send_all(sock, to_send, (int)strlen(to_send)) != 0) {
            fprintf(stderr,
                    "[ERROR] send() failed: %d\n"
                    "        The connection to the server may have been lost.\n",
                    WSAGetLastError());
            break;
        }

        printf("---\n");
        if (recv_response(sock) != 0) break;
        printf("---\n\n");
    }

    closesocket(sock);
    WSACleanup();
    return 0;
}