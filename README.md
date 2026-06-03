# USB/IP TCP Remote Control — Complete Guide

A TCP client/server application that lets a **Windows PC** control USB/IP
on a **Raspberry Pi (Linux)** over the network.

---

## Table of Contents

1. [Overview](#1-overview)
2. [Architecture](#2-architecture)
3. [Prerequisites](#3-prerequisites)
4. [File Structure](#4-file-structure)
5. [Server — Raspberry Pi (server.c)](#5-server--raspberry-pi-serverc)
6. [Client — Windows (client.c)](#6-client--windows-clientc)
7. [Build Instructions](#7-build-instructions)
8. [Running the Application](#8-running-the-application)
9. [Command Reference](#9-command-reference)
10. [Typical Workflow](#10-typical-workflow)
11. [Troubleshooting](#11-troubleshooting)
12. [Communication Protocol](#12-communication-protocol)
13. [Known Limitations](#13-known-limitations)

---

## 1. Overview

This project consists of two C programs:

| Component | Platform | Compiler | File |
|-----------|----------|----------|------|
| TCP Server | Raspberry Pi (Linux) | GCC | `server.c` |
| TCP Client | Windows PC | MSVC | `client.c` |

The Windows client connects to the Pi over TCP port **5000** and sends
text commands. The Pi server executes the corresponding USB/IP shell
commands and streams the output back to the client.

---

## 2. Architecture

```
Windows PC                            Raspberry Pi
───────────────                       ─────────────────────────────
usbip_client.exe
  │
  │  TCP :5000   "list_usb\n"
  ├─────────────────────────────────► usbip_server (main)
  │                                     │
  │                                     ├─ popen("usbip list -l")
  │                                     │   captures stdout + stderr
  │                                     │
  │  "output...\nEND_OF_RESPONSE\n"    │
  ◄─────────────────────────────────── send_response()
  │
  (prints to console)
```

**Protocol:** Plain text commands terminated by `\n`.
Responses end with the sentinel `END_OF_RESPONSE\n` so the client always
knows when a reply is complete — even for slow or multi-line output.

---

## 3. Prerequisites

### Raspberry Pi (Server side)

```bash
# Install USB/IP tools
sudo apt update
sudo apt install usbip linux-tools-generic

# Verify
usbip --version
usbipd --version
```

The server binary must be run as **root** (or via `sudo`) because
`modprobe` and `usbip bind` require root privileges.

### Windows PC (Client side)

- **Visual Studio** with MSVC (any version with C99 support), or
  **Build Tools for Visual Studio** (free download from Microsoft).
- **USB/IP for Windows** — required for the `local_*` commands:
  - Download from: https://github.com/dorssel/usbipd-win/releases
  - Install and ensure `usbip.exe` is on your system `PATH`.
- Run the client **as Administrator** for `local_bind_*` attach operations.

---

## 4. File Structure

```
project/
├── server.c              ← Raspberry Pi TCP server (GCC)
├── client.c              ← Windows TCP client (MSVC)
├── USBIP_TCP_GUIDE.md    ← This document
│
├── (Raspberry Pi build output)
│   └── build/main        ← compiled server binary
│
└── (Windows build output)
    └── usbip_client.exe  ← compiled client binary
```

---

## 5. Server — Raspberry Pi (`server.c`)

### What it does

- Listens on TCP port `5000` for incoming client connections.
- Accepts one client at a time (sequential, not concurrent).
- Reads newline-terminated text commands from the client.
- Executes the corresponding USB/IP shell command using `popen()`.
- Captures stdout **and** stderr (via `2>&1`) from each command.
- Sends the full output back, terminated by `END_OF_RESPONSE\n`.

### Key implementation details

| Feature | Detail |
|---------|--------|
| Socket options | `SO_REUSEADDR` + `SO_REUSEPORT` — allows immediate restart without "Address already in use" errors |
| SIGPIPE handling | Ignored — a disconnected client won't crash the server |
| `usbipd -D` | Launched via `system("sudo usbipd -D &")` — NOT `popen()`, because the daemon never exits. Verified with `pgrep` after 1 second |
| Bus ID validation | Only digits, `-`, and `.` accepted — prevents shell injection |
| Output buffer | 64 KiB per response |

### Source code

```c
/*
 * server.c - USB/IP TCP Server for Linux (Raspberry Pi)
 *
 * Build:
 *   gcc -Wall -Wextra -o usbip_server server.c
 *
 * Run (root required for usbip commands):
 *   sudo ./usbip_server
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>

#define SERVER_PORT      5000
#define BACKLOG          5
#define CMD_BUF_SIZE     256
#define OUT_BUF_SIZE     65536
#define SEND_CHUNK       4096

static int run_cmd(const char *cmd, char *buf, size_t bufsz)
{
    FILE *fp;
    size_t total = 0;
    char tmp[512];
    char full_cmd[512];
    snprintf(full_cmd, sizeof(full_cmd), "%s 2>&1", cmd);
    fp = popen(full_cmd, "r");
    if (!fp) {
        snprintf(buf, bufsz, "ERROR: popen failed: %s\n", strerror(errno));
        return -1;
    }
    buf[0] = '\0';
    while (fgets(tmp, sizeof(tmp), fp) != NULL) {
        size_t len = strlen(tmp);
        if (total + len + 1 < bufsz) {
            memcpy(buf + total, tmp, len);
            total += len;
            buf[total] = '\0';
        }
    }
    int rc = pclose(fp);
    if (rc == -1) return -1;
    return WEXITSTATUS(rc);
}

static int send_all(int sock, const char *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(sock, buf + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

static void send_response(int sock, const char *msg)
{
    send_all(sock, msg, strlen(msg));
    send_all(sock, "END_OF_RESPONSE\n", 16);
}

static void rtrim(char *s)
{
    int i = (int)strlen(s) - 1;
    while (i >= 0 && (s[i] == '\n' || s[i] == '\r' || s[i] == ' '))
        s[i--] = '\0';
}

static void cmd_usbserver_init(int sock)
{
    char result[OUT_BUF_SIZE] = "";
    char piece[OUT_BUF_SIZE / 2];
    char line[256];
    int  rc;

    rc = run_cmd("sudo modprobe usbip_core", piece, sizeof(piece));
    snprintf(line, sizeof(line), ">>> sudo modprobe usbip_core  (exit=%d)\n", rc);
    strncat(result, line, sizeof(result) - strlen(result) - 1);
    if (piece[0]) {
        strncat(result, piece, sizeof(result) - strlen(result) - 1);
        if (result[strlen(result)-1] != '\n')
            strncat(result, "\n", sizeof(result) - strlen(result) - 1);
    }
    strncat(result, "\n", sizeof(result) - strlen(result) - 1);

    rc = run_cmd("sudo modprobe usbip_host", piece, sizeof(piece));
    snprintf(line, sizeof(line), ">>> sudo modprobe usbip_host  (exit=%d)\n", rc);
    strncat(result, line, sizeof(result) - strlen(result) - 1);
    if (piece[0]) {
        strncat(result, piece, sizeof(result) - strlen(result) - 1);
        if (result[strlen(result)-1] != '\n')
            strncat(result, "\n", sizeof(result) - strlen(result) - 1);
    }
    strncat(result, "\n", sizeof(result) - strlen(result) - 1);

    strncat(result, ">>> sudo pkill -x usbipd  (cleanup old instance)\n",
            sizeof(result) - strlen(result) - 1);
    system("sudo pkill -x usbipd 2>/dev/null; sleep 0.3");

    strncat(result, ">>> sudo usbipd -D  (launching daemon in background)\n",
            sizeof(result) - strlen(result) - 1);
    rc = system("sudo usbipd -D &");
    snprintf(line, sizeof(line), "    system() returned: %d\n", rc);
    strncat(result, line, sizeof(result) - strlen(result) - 1);

    sleep(1);

    rc = run_cmd("pgrep -x usbipd", piece, sizeof(piece));
    if (rc == 0 && piece[0]) {
        char pid[32] = "";
        sscanf(piece, "%31s", pid);
        snprintf(line, sizeof(line), "    usbipd is running  (PID %s)\n\n", pid);
    } else {
        snprintf(line, sizeof(line),
                 "    WARNING: usbipd does not appear to be running!\n"
                 "    Try running manually: sudo usbipd -D\n\n");
    }
    strncat(result, line, sizeof(result) - strlen(result) - 1);
    send_response(sock, result);
}

static void cmd_list_usb(int sock)
{
    char out[OUT_BUF_SIZE];
    int rc = run_cmd("usbip list -l", out, sizeof(out));
    char hdr[64];
    snprintf(hdr, sizeof(hdr), ">>> usbip list -l  (exit=%d)\n", rc);
    char result[OUT_BUF_SIZE];
    snprintf(result, sizeof(result), "%s%s", hdr, out);
    send_response(sock, result);
}

static void cmd_bind_all(int sock)
{
    char list_out[OUT_BUF_SIZE];
    run_cmd("usbip list -l", list_out, sizeof(list_out));
    char result[OUT_BUF_SIZE] = "";
    char line[512];
    int found = 0;
    char *ptr = list_out;
    while (*ptr) {
        char *nl = strchr(ptr, '\n');
        size_t linelen = nl ? (size_t)(nl - ptr) : strlen(ptr);
        if (linelen >= sizeof(line)) linelen = sizeof(line) - 1;
        memcpy(line, ptr, linelen);
        line[linelen] = '\0';
        ptr += linelen + (nl ? 1 : 0);
        const char *marker = strstr(line, "busid ");
        if (!marker) continue;
        marker += 6;
        char busid[32];
        size_t bidlen = 0;
        while (*marker && *marker != ' ' && *marker != '(' && bidlen < sizeof(busid)-1)
            busid[bidlen++] = *marker++;
        busid[bidlen] = '\0';
        if (bidlen == 0) continue;
        int valid = 0;
        for (size_t j = 0; j < bidlen; j++) {
            if (busid[j] == '-') { valid = 1; break; }
        }
        if (!valid) continue;
        char bind_cmd[128];
        snprintf(bind_cmd, sizeof(bind_cmd), "sudo usbip bind -b %s", busid);
        char bind_out[4096];
        int rc = run_cmd(bind_cmd, bind_out, sizeof(bind_out));
        char entry[4096 + 128];
        snprintf(entry, sizeof(entry), ">>> %s  (exit=%d)\n%s\n", bind_cmd, rc, bind_out);
        strncat(result, entry, sizeof(result) - strlen(result) - 1);
        found++;
    }
    if (found == 0)
        strncat(result, "No USB devices found to bind.\n",
                sizeof(result) - strlen(result) - 1);
    send_response(sock, result);
}

static void cmd_bind_busid(int sock, const char *busid)
{
    for (const char *p = busid; *p; p++) {
        if (!isdigit((unsigned char)*p) && *p != '-' && *p != '.') {
            send_response(sock, "ERROR: Invalid bus ID format.\n");
            return;
        }
    }
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "sudo usbip bind -b %s", busid);
    char out[OUT_BUF_SIZE];
    int rc = run_cmd(cmd, out, sizeof(out));
    char result[OUT_BUF_SIZE];
    snprintf(result, sizeof(result), ">>> %s  (exit=%d)\n%s", cmd, rc, out);
    send_response(sock, result);
}

static void cmd_bind_list(int sock)
{
    char out[OUT_BUF_SIZE];
    int rc = run_cmd("usbip port", out, sizeof(out));
    char result[OUT_BUF_SIZE];
    snprintf(result, sizeof(result), ">>> usbip port  (exit=%d)\n%s", rc, out);
    send_response(sock, result);
}

static void dispatch(int sock, const char *cmd)
{
    printf("[server] Received command: '%s'\n", cmd);
    if (strcmp(cmd, "usbserver_init") == 0) {
        cmd_usbserver_init(sock);
    } else if (strcmp(cmd, "list_usb") == 0) {
        cmd_list_usb(sock);
    } else if (strcmp(cmd, "bind_all") == 0) {
        cmd_bind_all(sock);
    } else if (strcmp(cmd, "bind_list") == 0) {
        cmd_bind_list(sock);
    } else if (strncmp(cmd, "bind_", 5) == 0) {
        const char *busid = cmd + 5;
        if (*busid == '\0')
            send_response(sock, "ERROR: Missing bus ID after 'bind_'.\n");
        else
            cmd_bind_busid(sock, busid);
    } else {
        char err[CMD_BUF_SIZE + 64];
        snprintf(err, sizeof(err),
                 "ERROR: Unknown command '%s'.\n"
                 "Valid commands: usbserver_init, list_usb, bind_all, "
                 "bind_list, bind_<busid>\n", cmd);
        send_response(sock, err);
    }
}

static void handle_client(int client_fd, struct sockaddr_in *addr)
{
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip));
    printf("[server] Client connected: %s:%d\n", ip, ntohs(addr->sin_port));
    char buf[CMD_BUF_SIZE];
    int  pos = 0;
    while (1) {
        char c;
        ssize_t n = recv(client_fd, &c, 1, 0);
        if (n <= 0) break;
        if (c == '\n' || c == '\r') {
            if (pos == 0) continue;
            buf[pos] = '\0';
            rtrim(buf);
            if (pos > 0) dispatch(client_fd, buf);
            pos = 0;
        } else {
            if (pos < CMD_BUF_SIZE - 1)
                buf[pos++] = c;
        }
    }
    printf("[server] Client disconnected: %s:%d\n", ip, ntohs(addr->sin_port));
    close(client_fd);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(SERVER_PORT);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        fprintf(stderr, "  -> port %d may still be in use.\n", SERVER_PORT);
        fprintf(stderr, "     Run: sudo fuser -k %d/tcp  then retry.\n", SERVER_PORT);
        close(server_fd);
        return 1;
    }
    if (listen(server_fd, BACKLOG) < 0) { perror("listen"); close(server_fd); return 1; }
    printf("[server] USB/IP server listening on port %d\n", SERVER_PORT);
    printf("[server] Waiting for client connections...\n");

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) { perror("accept"); continue; }
        handle_client(client_fd, &client_addr);
    }
    close(server_fd);
    return 0;
}
```

---

## 6. Client — Windows (`client.c`)

### What it does

- Connects to the Pi server over TCP.
- Reads commands from the user (interactive prompt `usbip>`).
- **Remote commands** are forwarded to the server and the response is printed.
- **Local commands** (`local_*`) are executed directly on the Windows
  machine using `_popen()` — no server involvement needed.
- Detects the `END_OF_RESPONSE\n` sentinel across TCP packet boundaries
  (handles fragmentation correctly).

### Key implementation details

| Feature | Detail |
|---------|--------|
| Winsock | Version 2.2, linked with `Ws2_32.lib` |
| Sentinel detection | Sliding tail buffer handles sentinel split across two `recv()` calls |
| Local commands | Use `_popen()` with `2>&1` — stderr always visible |
| Bus ID validation | Digits, `-`, `.` only — prevents injection |
| `inet_pton` | Proper IPv4 parsing (no deprecated `inet_addr`) |

### Source code

```c
/*
 * client.c - USB/IP TCP Client for Windows
 *
 * Build with MSVC (Developer Command Prompt):
 *   cl client.c /W4 /Fe:usbip_client.exe Ws2_32.lib
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

#define DEFAULT_PORT     5000
#define CMD_BUF_SIZE     256
#define RECV_BUF_SIZE    4096
#define OUT_BUF_SIZE     65536
#define SENTINEL         "END_OF_RESPONSE\n"
#define MAX_BUSIDS       64
#define BUSID_LEN        32

static char g_server_ip[64] = {0};

static void rtrim(char *s) {
    int i = (int)strlen(s) - 1;
    while (i >= 0 && (s[i] == '\n' || s[i] == '\r' || s[i] == ' ')) s[i--] = '\0';
}

static int send_all(SOCKET sock, const char *buf, int len) {
    int sent = 0;
    while (sent < len) {
        int n = send(sock, buf + sent, len - sent, 0);
        if (n == SOCKET_ERROR) return -1;
        sent += n;
    }
    return 0;
}

static int recv_response(SOCKET sock) {
    char buf[RECV_BUF_SIZE + 1];
    char tail[sizeof(SENTINEL) + 1];
    int  tail_len = 0;
    memset(tail, 0, sizeof(tail));
    while (1) {
        int n = recv(sock, buf, RECV_BUF_SIZE, 0);
        if (n == SOCKET_ERROR) { fprintf(stderr, "[ERROR] recv failed: %d\n", WSAGetLastError()); return -1; }
        if (n == 0) { fprintf(stderr, "[ERROR] Server closed connection unexpectedly.\n"); return -1; }
        buf[n] = '\0';
        char window[sizeof(SENTINEL) + RECV_BUF_SIZE + 1];
        int  wlen = tail_len + n;
        memcpy(window, tail, tail_len);
        memcpy(window + tail_len, buf, n + 1);
        char *end = strstr(window, SENTINEL);
        if (end) {
            *end = '\0';
            if (end > window + tail_len)
                fwrite(window + tail_len, 1, (size_t)(end - window - tail_len), stdout);
            fflush(stdout);
            return 0;
        }
        int sentinel_len = (int)(sizeof(SENTINEL) - 1);
        int safe = wlen - sentinel_len;
        if (safe < 0) safe = 0;
        if (safe > tail_len) { fwrite(window + tail_len, 1, (size_t)(safe - tail_len), stdout); fflush(stdout); }
        if (sentinel_len > wlen) sentinel_len = wlen;
        tail_len = sentinel_len;
        memcpy(tail, window + wlen - tail_len, (size_t)tail_len);
        tail[tail_len] = '\0';
    }
}

static int local_run(const char *cmd) {
    printf(">>> %s\n", cmd); fflush(stdout);
    char full[512];
    snprintf(full, sizeof(full), "%s 2>&1", cmd);
    FILE *fp = _popen(full, "r");
    if (!fp) { fprintf(stderr, "[ERROR] Failed to execute: %s\n        Make sure usbip.exe is installed and on PATH.\n", cmd); return -1; }
    char line[512]; int got_output = 0;
    while (fgets(line, sizeof(line), fp)) { fputs(line, stdout); got_output = 1; }
    if (!got_output) printf("(no output)\n");
    int rc = _pclose(fp);
    printf("(exit code: %d)\n", rc); fflush(stdout);
    return rc;
}

static int parse_remote_busids(const char *output, char busids[][BUSID_LEN], int max) {
    int count = 0;
    const char *p = output;
    while (*p && count < max) {
        const char *line_start = p;
        while (*p == ' ' || *p == '\t') p++;
        if (p > line_start && isdigit((unsigned char)*p)) {
            char token[BUSID_LEN]; int tlen = 0;
            while (*p && *p != ':' && *p != ' ' && tlen < BUSID_LEN - 1) token[tlen++] = *p++;
            token[tlen] = '\0';
            if (tlen > 0 && strchr(token, '-')) { strncpy(busids[count], token, BUSID_LEN - 1); busids[count][BUSID_LEN-1] = '\0'; count++; }
        }
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    return count;
}

static void cmd_local_list(void) {
    char cmd[128]; snprintf(cmd, sizeof(cmd), "usbip list -r %s", g_server_ip); local_run(cmd);
}

static void cmd_local_bind_busid(const char *busid) {
    for (const char *p = busid; *p; p++) {
        if (!isdigit((unsigned char)*p) && *p != '-' && *p != '.') { fprintf(stderr, "[ERROR] Invalid bus ID: '%s'\n", busid); return; }
    }
    char cmd[256]; snprintf(cmd, sizeof(cmd), "usbip attach -r %s -b %s", g_server_ip, busid); local_run(cmd);
}

static void cmd_local_bind_all(void) {
    char list_cmd[128]; snprintf(list_cmd, sizeof(list_cmd), "usbip list -r %s 2>&1", g_server_ip);
    printf(">>> %s\n", list_cmd); fflush(stdout);
    FILE *fp = _popen(list_cmd, "r");
    if (!fp) { fprintf(stderr, "[ERROR] Failed to run usbip list -r %s\n        Make sure usbip.exe is installed and on PATH.\n", g_server_ip); return; }
    char output[OUT_BUF_SIZE]; size_t total = 0; char line[512];
    while (fgets(line, sizeof(line), fp) && total + strlen(line) < OUT_BUF_SIZE - 1) { fputs(line, stdout); strcat(output + total, line); total += strlen(line); }
    _pclose(fp); fflush(stdout);
    char busids[MAX_BUSIDS][BUSID_LEN];
    int count = parse_remote_busids(output, busids, MAX_BUSIDS);
    if (count == 0) { printf("[local_bind_all] No remote USB devices found to attach.\n"); return; }
    printf("\n[local_bind_all] Found %d device(s). Attaching...\n\n", count);
    for (int i = 0; i < count; i++) {
        char attach_cmd[256]; snprintf(attach_cmd, sizeof(attach_cmd), "usbip attach -r %s -b %s", g_server_ip, busids[i]);
        local_run(attach_cmd); printf("\n");
    }
}

static void print_help(void) {
    printf("\n");
    printf("  +----------------------------------------------------------------+\n");
    printf("  |           USB/IP Client Command Reference                      |\n");
    printf("  +------------------+---------------------------------------------+\n");
    printf("  | REMOTE COMMANDS  | sent to Pi server at %-22s |\n", g_server_ip);
    printf("  +------------------+---------------------------------------------+\n");
    printf("  |  usbserver_init     load kernel modules + start usbipd         |\n");
    printf("  |  list_usb           list USB devices on the Pi                 |\n");
    printf("  |  bind_all           bind all Pi USB devices for export          |\n");
    printf("  |  bind_list          show currently bound devices on Pi          |\n");
    printf("  |  bind_<busid>       bind specific device  e.g. bind_1-1        |\n");
    printf("  +----------------------------------------------------------------+\n");
    printf("  | LOCAL COMMANDS   (run on THIS Windows machine)                 |\n");
    printf("  +----------------------------------------------------------------+\n");
    printf("  |  local_list         usbip list -r <server_ip>                  |\n");
    printf("  |  local_bind_all     attach all remote devices to Windows        |\n");
    printf("  |  local_bind_<id>    usbip attach -r <ip> -b <busid>            |\n");
    printf("  |                     e.g. local_bind_1-1.1                      |\n");
    printf("  +----------------------------------------------------------------+\n");
    printf("  |  help               show this help                             |\n");
    printf("  |  exit / quit        disconnect and close                       |\n");
    printf("  +----------------------------------------------------------------+\n");
    printf("\n");
}

int main(int argc, char *argv[]) {
    if (argc < 2) { fprintf(stderr, "Usage: %s <server_ip> [port]\n  Default port: %d\n", argv[0], DEFAULT_PORT); return 1; }
    strncpy(g_server_ip, argv[1], sizeof(g_server_ip) - 1);
    int port = (argc >= 3) ? atoi(argv[2]) : DEFAULT_PORT;
    if (port <= 0 || port > 65535) { fprintf(stderr, "Invalid port: %s\n", argv[2]); return 1; }

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { fprintf(stderr, "WSAStartup failed: %d\n", WSAGetLastError()); return 1; }

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) { fprintf(stderr, "socket() failed: %d\n", WSAGetLastError()); WSACleanup(); return 1; }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port   = htons((u_short)port);
    if (inet_pton(AF_INET, g_server_ip, &server_addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid server IP address: %s\n", g_server_ip); closesocket(sock); WSACleanup(); return 1;
    }
    printf("[client] Connecting to %s:%d ...\n", g_server_ip, port);
    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        fprintf(stderr, "[ERROR] connect() failed: %d\n        Check that the server is running and the IP/port are correct.\n", WSAGetLastError());
        closesocket(sock); WSACleanup(); return 1;
    }
    printf("[client] Connected to %s:%d\n", g_server_ip, port);
    print_help();

    char cmd[CMD_BUF_SIZE];
    while (1) {
        printf("usbip> "); fflush(stdout);
        if (!fgets(cmd, sizeof(cmd), stdin)) break;
        rtrim(cmd);
        if (cmd[0] == '\0') continue;
        if (_stricmp(cmd, "exit") == 0 || _stricmp(cmd, "quit") == 0) { printf("[client] Disconnecting.\n"); break; }
        if (_stricmp(cmd, "help") == 0) { print_help(); continue; }
        if (_stricmp(cmd, "local_list") == 0) { printf("---\n"); cmd_local_list(); printf("---\n\n"); continue; }
        if (_stricmp(cmd, "local_bind_all") == 0) { printf("---\n"); cmd_local_bind_all(); printf("---\n\n"); continue; }
        if (_strnicmp(cmd, "local_bind_", 11) == 0) {
            const char *busid = cmd + 11;
            if (*busid == '\0') fprintf(stderr, "[ERROR] Missing bus ID. Usage: local_bind_<busid>  e.g. local_bind_1-1.1\n");
            else { printf("---\n"); cmd_local_bind_busid(busid); printf("---\n\n"); }
            continue;
        }
        if (_strnicmp(cmd, "local", 5) == 0) {
            fprintf(stderr, "[ERROR] Unknown local command '%s'.\n        Local commands: local_list, local_bind_all, local_bind_<busid>\n", cmd);
            continue;
        }
        char to_send[CMD_BUF_SIZE + 2];
        snprintf(to_send, sizeof(to_send), "%s\n", cmd);
        if (send_all(sock, to_send, (int)strlen(to_send)) != 0) {
            fprintf(stderr, "[ERROR] send() failed: %d\n        The connection to the server may have been lost.\n", WSAGetLastError()); break;
        }
        printf("---\n");
        if (recv_response(sock) != 0) break;
        printf("---\n\n");
    }
    closesocket(sock);
    WSACleanup();
    return 0;
}
```

---

## 7. Build Instructions

### 7.1 Server — Raspberry Pi

```bash
# Compile
gcc -Wall -Wextra -o build/main server.c

# Or with your build script
./build.sh
```

### 7.2 Client — Windows (MSVC)

Open a **Developer Command Prompt for Visual Studio**, then:

```cmd
cl client.c /W4 /Fe:usbip_client.exe Ws2_32.lib
```

Or with a build batch file `build.bat`:

```bat
@echo off
cl client.c /W4 /Fe:usbip_client.exe Ws2_32.lib
if %ERRORLEVEL% == 0 (
    echo Build successful: usbip_client.exe
) else (
    echo Build FAILED
)
```

---

## 8. Running the Application

### 8.1 Start the server (Raspberry Pi)

```bash
# Using your run script
./run.sh

# Or directly
sudo ./build/main
```

Expected output:
```
[server] USB/IP server listening on port 5000
[server] Waiting for client connections...
```

### 8.2 If you get "bind: Address already in use"

```bash
# Kill everything on port 5000 by port number (not PID)
sudo fuser -k 5000/tcp

# Verify it's clear
sudo ss -tlnp | grep 5000

# Now start
./run.sh
```

### 8.3 Start the client (Windows)

Run **as Administrator** (required for `local_bind_*` attach commands):

```cmd
usbip_client.exe 192.168.0.15
```

With explicit port:

```cmd
usbip_client.exe 192.168.0.15 5000
```

---

## 9. Command Reference

### Remote Commands (forwarded to the Pi server)

| Command | Pi shell commands executed | Description |
|---------|---------------------------|-------------|
| `usbserver_init` | `modprobe usbip_core`<br>`modprobe usbip_host`<br>`usbipd -D &` | Load kernel modules and start the USB/IP daemon |
| `list_usb` | `usbip list -l` | List all USB devices available on the Pi |
| `bind_all` | `usbip list -l` then<br>`usbip bind -b <id>` for each | Bind every detected USB device for export |
| `bind_list` | `usbip port` | Show currently bound/exported devices |
| `bind_<busid>` | `usbip bind -b <busid>` | Bind one specific device, e.g. `bind_1-1` |

### Local Commands (run on Windows, no server contact)

| Command | Windows command executed | Description |
|---------|--------------------------|-------------|
| `local_list` | `usbip list -r 192.168.0.15` | List all exported devices on the Pi |
| `local_bind_all` | `usbip list -r ...` then<br>`usbip attach -r ... -b <id>` for each | Attach every remote device to Windows |
| `local_bind_<busid>` | `usbip attach -r 192.168.0.15 -b <busid>` | Attach one device, e.g. `local_bind_1-1.1` |

### Utility Commands

| Command | Description |
|---------|-------------|
| `help` | Print the command reference table |
| `exit` / `quit` | Disconnect and close the client |

---

## 10. Typical Workflow

```
Step 1 — Initialise the Pi
  usbip> usbserver_init
      Loads usbip_core, usbip_host kernel modules.
      Starts usbipd daemon in the background.
      Reports PID if successful.

Step 2 — See what USB devices are on the Pi
  usbip> list_usb
      Runs: usbip list -l
      Shows bus IDs like 1-1, 1-1.1, 2-1 etc.

Step 3 — Export (bind) the devices
  usbip> bind_all              <- bind everything at once
  -- or --
  usbip> bind_1-1              <- bind one specific device

Step 4 — Confirm devices are exported
  usbip> bind_list
      Runs: usbip port
      Shows which devices are currently bound/exported.

Step 5 — From Windows, see available devices
  usbip> local_list
      Runs locally: usbip list -r 192.168.0.15
      Shows exported devices ready to attach.

Step 6 — Attach to Windows
  usbip> local_bind_all            <- attach everything
  -- or --
  usbip> local_bind_1-1.1          <- attach one device
```

---

## 11. Troubleshooting

### "bind: Address already in use" on the Pi

```bash
# Force-kill all processes using port 5000
sudo fuser -k 5000/tcp

# Wait a moment, then restart
./run.sh
```

> The updated server uses both `SO_REUSEADDR` and `SO_REUSEPORT` so this
> should only happen if a **different** process is holding the port.

### Client hangs after `usbserver_init`

This was caused by `usbipd -D` never exiting. Fixed in the current
`server.c` — the daemon is now launched with `system("sudo usbipd -D &")`
so the shell detaches it immediately. The server then waits 1 second and
confirms via `pgrep` before responding.

### `usbserver_init` reports "WARNING: usbipd does not appear to be running"

```bash
# Check if usbipd is installed
which usbipd

# Install if missing
sudo apt install usbip

# Try running manually
sudo usbipd -D
```

### `local_*` commands fail with "Failed to execute"

- Ensure `usbip.exe` (usbipd-win) is installed on the Windows machine.
- Confirm it is on your system `PATH`: open a new CMD and run `usbip --version`.
- Run the client as **Administrator**.

### Cannot connect from Windows to Pi

```bash
# On the Pi — check server is listening
sudo ss -tlnp | grep 5000

# Check firewall
sudo ufw status
sudo ufw allow 5000/tcp   # if needed
```

On Windows:
```cmd
# Test connectivity
ping 192.168.0.15
telnet 192.168.0.15 5000
```

### Permission denied on Pi for usbip commands

The server must run as root:

```bash
sudo ./build/main
```

Or configure passwordless sudo for `usbip` and `modprobe` in `/etc/sudoers`.

---

## 12. Communication Protocol

```
Client                              Server
──────                              ──────
"list_usb\n"          ──────────►
                      ◄──────────  ">>> usbip list -l  (exit=0)\n"
                                   "- busid 1-1 ...\n"
                                   "- busid 1-2 ...\n"
                      ◄──────────  "END_OF_RESPONSE\n"
```

- Commands: plain text, terminated by `\n`
- Responses: plain text, any length, terminated by `END_OF_RESPONSE\n`
- The sentinel is detected across TCP packet boundaries using a sliding
  tail buffer in `recv_response()` — so it works correctly even if the
  sentinel is split across two `recv()` calls.

---

## 13. Known Limitations

- **One client at a time** — the server handles clients sequentially.
  To support concurrent clients, `handle_client()` would need to be run
  in a thread or forked process.
- **No authentication** — any machine that can reach port 5000 can send
  commands. Use firewall rules to restrict access to trusted IPs only.
- **Plain text** — commands and responses are unencrypted. For production
  use over untrusted networks, wrap the connection in TLS.
- **64 KiB response buffer** — very long `usbip` output will be truncated.
  Increase `OUT_BUF_SIZE` in `server.c` if needed.