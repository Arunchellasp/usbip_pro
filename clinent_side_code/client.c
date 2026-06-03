/*
 * client.c - USB/IP TCP Client for Windows
 *
 * Build with MSVC (Developer Command Prompt):
 *   cl client.c /W4 /Fe:usbip_client.exe Ws2_32.lib
 *
 * Usage:
 *   usbip_client.exe <server_ip> [port]    (default port: 5000)
 *
 * ── Remote commands (sent to server over TCP) ──────────────────────────────
 *   usbserver_init          load kernel modules + start usbipd on the Pi
 *   list_usb                usbip list -l  (local list on the Pi)
 *   bind_all                bind every USB device on the Pi
 *   bind_list               show currently bound devices on the Pi
 *   bind_<busid>            bind a specific device, e.g. bind_1-1
 *
 * ── Local commands (executed on THIS Windows machine, no server needed) ────
 *   local_list              runs: usbip list -r <server_ip>
 *   local_bind_all          lists remote devices then attaches every one:
 *                             usbip attach -r <server_ip> -b <busid>
 *   local_bind_<busid>      attach one device, e.g. local_bind_1-1.1
 *                             usbip attach -r <server_ip> -b <busid>
 *
 * ── Other ──────────────────────────────────────────────────────────────────
 *   help                    print this command list
 *   exit / quit             disconnect and close
 *
 * NOTE: local_* commands require usbip.exe (Windows USB/IP client) to be
 *       installed and on the system PATH, and the shell must be run as
 *       Administrator for attach operations.
 */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600   /* Vista+ for inet_pton */
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#pragma comment(lib, "Ws2_32.lib")

/* ── Configuration ─────────────────────────────────────────────── */
#define DEFAULT_PORT     5000
#define CMD_BUF_SIZE     256
#define RECV_BUF_SIZE    4096
#define OUT_BUF_SIZE     65536
#define SENTINEL         "END_OF_RESPONSE\n"
#define MAX_BUSIDS       64
#define BUSID_LEN        32

/* Server IP stored globally so local commands can use it */
static char g_server_ip[64] = {0};

/* ── Helpers ────────────────────────────────────────────────────── */

static void rtrim(char *s)
{
    int i = (int)strlen(s) - 1;
    while (i >= 0 && (s[i] == '\n' || s[i] == '\r' || s[i] == ' '))
        s[i--] = '\0';
}

/* Send all bytes (handles partial sends). Returns 0 on success, -1 on error. */
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
 * Receive the server's response until the sentinel "END_OF_RESPONSE\n"
 * is found, printing data as it arrives.
 * Returns 0 on success, -1 on socket error / disconnect.
 */
static int recv_response(SOCKET sock)
{
    char buf[RECV_BUF_SIZE + 1];
    char tail[sizeof(SENTINEL) + 1];
    int  tail_len = 0;
    memset(tail, 0, sizeof(tail));

    while (1) {
        int n = recv(sock, buf, RECV_BUF_SIZE, 0);
        if (n == SOCKET_ERROR) {
            fprintf(stderr, "[ERROR] recv failed: %d\n", WSAGetLastError());
            return -1;
        }
        if (n == 0) {
            fprintf(stderr, "[ERROR] Server closed connection unexpectedly.\n");
            return -1;
        }
        buf[n] = '\0';

        char window[sizeof(SENTINEL) + RECV_BUF_SIZE + 1];
        int  wlen = tail_len + n;
        memcpy(window, tail, tail_len);
        memcpy(window + tail_len, buf, n + 1);

        char *end = strstr(window, SENTINEL);
        if (end) {
            *end = '\0';
            if (end > window + tail_len)
                fwrite(window + tail_len, 1,
                       (size_t)(end - window - tail_len), stdout);
            fflush(stdout);
            return 0;
        }

        int sentinel_len = (int)(sizeof(SENTINEL) - 1);
        int safe = wlen - sentinel_len;
        if (safe < 0) safe = 0;
        if (safe > tail_len) {
            fwrite(window + tail_len, 1, (size_t)(safe - tail_len), stdout);
            fflush(stdout);
        }

        if (sentinel_len > wlen) sentinel_len = wlen;
        tail_len = sentinel_len;
        memcpy(tail, window + wlen - tail_len, (size_t)tail_len);
        tail[tail_len] = '\0';
    }
}

/* ── Local command helpers ──────────────────────────────────────── */

/*
 * Run a local Windows command via cmd.exe and print its output + exit code.
 * stderr is merged into stdout.
 * Returns the process exit code, or -1 on popen failure.
 */
static int local_run(const char *cmd)
{
    printf(">>> %s\n", cmd);
    fflush(stdout);

    char full[512];
    snprintf(full, sizeof(full), "%s 2>&1", cmd);

    FILE *fp = _popen(full, "r");
    if (!fp) {
        fprintf(stderr, "[ERROR] Failed to execute: %s\n", cmd);
        fprintf(stderr, "        Make sure usbip.exe is installed and on PATH.\n");
        return -1;
    }

    char line[512];
    int  got_output = 0;
    while (fgets(line, sizeof(line), fp)) {
        fputs(line, stdout);
        got_output = 1;
    }
    if (!got_output)
        printf("(no output)\n");

    int rc = _pclose(fp);
    printf("(exit code: %d)\n", rc);
    fflush(stdout);
    return rc;
}

/*
 * Parse the output of "usbip list -r <ip>" and collect all bus IDs.
 * Lines of interest look like:
 *        1-1.1: Some Device Manufacturer (xxxx:yyyy)
 * The bus ID is the token before the first colon on indented lines.
 *
 * Returns the number of bus IDs found.
 */
static int parse_remote_busids(const char *output,
                                char busids[][BUSID_LEN], int max)
{
    int count = 0;
    const char *p = output;

    while (*p && count < max) {
        /* Skip leading spaces/tabs to check indentation */
        const char *line_start = p;
        while (*p == ' ' || *p == '\t') p++;

        /* An indented line that starts with a digit is a device entry */
        if (p > line_start && isdigit((unsigned char)*p)) {
            /* Collect token up to ':' or space */
            char token[BUSID_LEN];
            int  tlen = 0;
            while (*p && *p != ':' && *p != ' ' && tlen < BUSID_LEN - 1)
                token[tlen++] = *p++;
            token[tlen] = '\0';

            /* Must contain a '-' to be a valid bus ID like 1-1 or 1-1.1 */
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

/* ── Local command: local_list ──────────────────────────────────── */
static void cmd_local_list(void)
{
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "usbip list -r %s", g_server_ip);
    local_run(cmd);
}

/* ── Local command: local_bind_<busid> ─────────────────────────── */
static void cmd_local_bind_busid(const char *busid)
{
    /* Validate: digits, hyphens, dots only */
    for (const char *p = busid; *p; p++) {
        if (!isdigit((unsigned char)*p) && *p != '-' && *p != '.') {
            fprintf(stderr, "[ERROR] Invalid bus ID: '%s'\n", busid);
            return;
        }
    }

    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "usbip attach -r %s -b %s", g_server_ip, busid);
    local_run(cmd);
}

/* ── Local command: local_bind_all ─────────────────────────────── */
static void cmd_local_bind_all(void)
{
    /* Step 1: run usbip list -r and capture output */
    char list_cmd[128];
    snprintf(list_cmd, sizeof(list_cmd), "usbip list -r %s 2>&1", g_server_ip);

    printf(">>> %s\n", list_cmd);
    fflush(stdout);

    FILE *fp = _popen(list_cmd, "r");
    if (!fp) {
        fprintf(stderr, "[ERROR] Failed to run usbip list -r %s\n", g_server_ip);
        fprintf(stderr, "        Make sure usbip.exe is installed and on PATH.\n");
        return;
    }

    char output[OUT_BUF_SIZE];
    size_t total = 0;
    char line[512];
    while (fgets(line, sizeof(line), fp) && total + strlen(line) < OUT_BUF_SIZE - 1) {
        fputs(line, stdout);
        strcat(output + total, line);
        total += strlen(line);
    }
    _pclose(fp);
    fflush(stdout);

    /* Step 2: parse bus IDs */
    char busids[MAX_BUSIDS][BUSID_LEN];
    int count = parse_remote_busids(output, busids, MAX_BUSIDS);

    if (count == 0) {
        printf("[local_bind_all] No remote USB devices found to attach.\n");
        return;
    }

    printf("\n[local_bind_all] Found %d device(s). Attaching...\n\n", count);

    /* Step 3: attach each one */
    for (int i = 0; i < count; i++) {
        char attach_cmd[256];
        snprintf(attach_cmd, sizeof(attach_cmd),
                 "usbip attach -r %s -b %s", g_server_ip, busids[i]);
        local_run(attach_cmd);
        printf("\n");
    }
}

/* ── Help ───────────────────────────────────────────────────────── */
static void print_help(void)
{
    printf("\n");
    printf("  ╔══════════════════════════════════════════════════════════════╗\n");
    printf("  ║              USB/IP Client Command Reference                 ║\n");
    printf("  ╠══════════════════╦═══════════════════════════════════════════╣\n");
    printf("  ║  REMOTE COMMANDS ║ (sent to Pi server at %s)          \n", g_server_ip);
    printf("  ╠══════════════════╩═══════════════════════════════════════════╣\n");
    printf("  ║  usbserver_init     load kernel modules + start usbipd       ║\n");
    printf("  ║  list_usb           list USB devices on the Pi               ║\n");
    printf("  ║  bind_all           bind all Pi USB devices for export        ║\n");
    printf("  ║  bind_list          show currently bound devices on Pi        ║\n");
    printf("  ║  bind_<busid>       bind specific device  e.g. bind_1-1      ║\n");
    printf("  ╠══════════════════════════════════════════════════════════════╣\n");
    printf("  ║  LOCAL COMMANDS  (run on THIS Windows machine)               ║\n");
    printf("  ╠══════════════════════════════════════════════════════════════╣\n");
    printf("  ║  local_list         usbip list -r <server_ip>                ║\n");
    printf("  ║  local_bind_all     attach all remote devices to Windows      ║\n");
    printf("  ║  local_bind_<id>    usbip attach -r <ip> -b <busid>          ║\n");
    printf("  ║                     e.g. local_bind_1-1.1                    ║\n");
    printf("  ╠══════════════════════════════════════════════════════════════╣\n");
    printf("  ║  help               show this help                           ║\n");
    printf("  ║  exit / quit        disconnect and close                     ║\n");
    printf("  ╚══════════════════════════════════════════════════════════════╝\n");
    printf("\n");
}

/* ── main ───────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr,
                "Usage: %s <server_ip> [port]\n"
                "  Default port: %d\n", argv[0], DEFAULT_PORT);
        return 1;
    }

    strncpy(g_server_ip, argv[1], sizeof(g_server_ip) - 1);
    int port = (argc >= 3) ? atoi(argv[2]) : DEFAULT_PORT;
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port: %s\n", argv[2]);
        return 1;
    }

    /* ── Initialise Winsock ──────────────────────────────────────── */
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed: %d\n", WSAGetLastError());
        return 1;
    }

    /* ── Create socket ───────────────────────────────────────────── */
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        fprintf(stderr, "socket() failed: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    /* ── Connect to server ───────────────────────────────────────── */
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port   = htons((u_short)port);

    if (inet_pton(AF_INET, g_server_ip, &server_addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid server IP address: %s\n", g_server_ip);
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    printf("[client] Connecting to %s:%d ...\n", g_server_ip, port);

    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr))
            == SOCKET_ERROR) {
        fprintf(stderr, "[ERROR] connect() failed: %d\n"
                        "        Check that the server is running and the IP/port are correct.\n",
                WSAGetLastError());
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    printf("[client] Connected to %s:%d\n", g_server_ip, port);
    print_help();

    /* ── Command loop ────────────────────────────────────────────── */
    char cmd[CMD_BUF_SIZE];

    while (1) {
        printf("usbip> ");
        fflush(stdout);

        if (!fgets(cmd, sizeof(cmd), stdin)) break;
        rtrim(cmd);
        if (cmd[0] == '\0') continue;

        /* ── exit / quit ─────────────────────────────────────────── */
        if (_stricmp(cmd, "exit") == 0 || _stricmp(cmd, "quit") == 0) {
            printf("[client] Disconnecting.\n");
            break;
        }

        /* ── help ────────────────────────────────────────────────── */
        if (_stricmp(cmd, "help") == 0) {
            print_help();
            continue;
        }

        /* ══════════════════════════════════════════════════════════
         *  LOCAL COMMANDS — handled entirely on the Windows side
         * ══════════════════════════════════════════════════════════ */

        if (_stricmp(cmd, "local_list") == 0) {
            printf("---\n");
            cmd_local_list();
            printf("---\n\n");
            continue;
        }

        if (_stricmp(cmd, "local_bind_all") == 0) {
            printf("---\n");
            cmd_local_bind_all();
            printf("---\n\n");
            continue;
        }

        if (_strnicmp(cmd, "local_bind_", 11) == 0) {
            const char *busid = cmd + 11;
            if (*busid == '\0') {
                fprintf(stderr, "[ERROR] Missing bus ID. Usage: local_bind_<busid>"
                                "  e.g. local_bind_1-1.1\n");
            } else {
                printf("---\n");
                cmd_local_bind_busid(busid);
                printf("---\n\n");
            }
            continue;
        }

        /* ══════════════════════════════════════════════════════════
         *  REMOTE COMMANDS — forwarded to the Pi server
         * ══════════════════════════════════════════════════════════ */

        /* Basic guard: reject commands that look like local_ typos */
        if (_strnicmp(cmd, "local", 5) == 0) {
            fprintf(stderr, "[ERROR] Unknown local command '%s'.\n"
                            "        Local commands: local_list, local_bind_all,"
                            " local_bind_<busid>\n", cmd);
            continue;
        }

        char to_send[CMD_BUF_SIZE + 2];
        snprintf(to_send, sizeof(to_send), "%s\n", cmd);

        if (send_all(sock, to_send, (int)strlen(to_send)) != 0) {
            fprintf(stderr, "[ERROR] send() failed: %d\n"
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