/*
 * server.c  –  USB/IP TCP Server for Linux (Raspberry Pi)
 *
 * Build:
 *   gcc -Wall -Wextra -O2 -o usbip_server server.c
 *
 * Run (root required for usbip commands):
 *   sudo ./usbip_server
 *
 * For auto-start on boot, install the provided usbip_server.service
 * file as a systemd unit (see README / usbip_server.service).
 *
 * Listens on TCP port 5000.  One client at a time; commands are
 * newline-terminated text strings.  Every reply ends with the
 * sentinel "END_OF_RESPONSE\n" so the client knows it is complete.
 */

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* ── Configuration ─────────────────────────────────────────────── */
#define SERVER_PORT   5000
#define BACKLOG       5
#define CMD_BUF_SIZE  256
#define OUT_BUF_SIZE  65536   /* 64 KiB – max captured command output */

/* ── Utility: run a shell command, capture stdout+stderr ────────── */
/*
 * Returns the command's exit code, or -1 on popen() failure.
 * Output is always NUL-terminated; truncated silently if > bufsz-1.
 */
static int run_cmd(const char *cmd, char *buf, size_t bufsz)
{
    char full_cmd[512];
    snprintf(full_cmd, sizeof(full_cmd), "%s 2>&1", cmd);

    FILE *fp = popen(full_cmd, "r");
    if (!fp) {
        snprintf(buf, bufsz, "ERROR: popen failed: %s\n", strerror(errno));
        return -1;
    }

    size_t total = 0;
    buf[0] = '\0';
    char tmp[512];
    while (fgets(tmp, sizeof(tmp), fp)) {
        size_t len = strlen(tmp);
        if (total + len + 1 < bufsz) {
            memcpy(buf + total, tmp, len);
            total += len;
            buf[total] = '\0';
        }
    }

    int rc = pclose(fp);
    return (rc == -1) ? -1 : WEXITSTATUS(rc);
}

/* ── Utility: send all bytes (handles partial writes) ───────────── */
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

/* ── Utility: send reply + end-of-response sentinel ────────────── */
static void send_response(int sock, const char *msg)
{
    send_all(sock, msg, strlen(msg));
    send_all(sock, "END_OF_RESPONSE\n", 16);
}

/* ── Utility: strip trailing whitespace / newline in-place ─────── */
static void rtrim(char *s)
{
    int i = (int)strlen(s) - 1;
    while (i >= 0 && (s[i] == '\n' || s[i] == '\r' || s[i] == ' '))
        s[i--] = '\0';
}

/* ── Utility: append a string to a fixed buffer (safe) ─────────── */
static void buf_append(char *dst, size_t dstsz, const char *src)
{
    size_t used = strlen(dst);
    if (used + 1 >= dstsz) return;
    strncat(dst, src, dstsz - used - 1);
}

/* ── Command: usbserver_init ────────────────────────────────────── */
/*
 * Load kernel modules and start the USB/IP daemon.
 *
 * usbipd -D forks itself into the background, so we MUST NOT call it
 * via popen() – that would block forever.  We use system() with "&"
 * instead, wait one second, then verify with pgrep.
 */
static void cmd_usbserver_init(int sock)
{
    char result[OUT_BUF_SIZE] = "";
    char piece[OUT_BUF_SIZE / 2];
    char line[256];
    int  rc;

    /* 1. modprobe usbip_core */
    rc = run_cmd("sudo modprobe usbip_core", piece, sizeof(piece));
    snprintf(line, sizeof(line), ">>> sudo modprobe usbip_core  (exit=%d)\n", rc);
    buf_append(result, sizeof(result), line);
    if (piece[0]) {
        buf_append(result, sizeof(result), piece);
        if (result[strlen(result) - 1] != '\n')
            buf_append(result, sizeof(result), "\n");
    }
    buf_append(result, sizeof(result), "\n");

    /* 2. modprobe usbip_host */
    rc = run_cmd("sudo modprobe usbip_host", piece, sizeof(piece));
    snprintf(line, sizeof(line), ">>> sudo modprobe usbip_host  (exit=%d)\n", rc);
    buf_append(result, sizeof(result), line);
    if (piece[0]) {
        buf_append(result, sizeof(result), piece);
        if (result[strlen(result) - 1] != '\n')
            buf_append(result, sizeof(result), "\n");
    }
    buf_append(result, sizeof(result), "\n");

    /* 3. Kill any stale usbipd, then launch fresh daemon in background */
    buf_append(result, sizeof(result),
               ">>> sudo pkill -x usbipd  (cleanup old instance)\n");
    (void)system("sudo pkill -x usbipd 2>/dev/null; sleep 0.3");

    buf_append(result, sizeof(result),
               ">>> sudo usbipd -D  (launching daemon in background)\n");
    rc = system("sudo usbipd -D &");  /* rc intentionally checked below */
    snprintf(line, sizeof(line), "    system() returned: %d\n", rc);
    buf_append(result, sizeof(result), line);

    sleep(1);

    /* 4. Verify daemon is up */
    rc = run_cmd("pgrep -x usbipd", piece, sizeof(piece));
    if (rc == 0 && piece[0]) {
        char pid[32] = "";
        sscanf(piece, "%31s", pid);
        snprintf(line, sizeof(line),
                 "    usbipd is running  (PID %s)\n\n", pid);
    } else {
        snprintf(line, sizeof(line),
                 "    WARNING: usbipd does not appear to be running!\n"
                 "    Try: sudo usbipd -D\n\n");
    }
    buf_append(result, sizeof(result), line);

    send_response(sock, result);
}

/* ── Command: list_usb ──────────────────────────────────────────── */
static void cmd_list_usb(int sock)
{
    char out[OUT_BUF_SIZE];
    int  rc = run_cmd("usbip list -l", out, sizeof(out));

    char result[OUT_BUF_SIZE];
    snprintf(result, sizeof(result), ">>> usbip list -l  (exit=%d)\n%s", rc, out);
    send_response(sock, result);
}

/* ── Command: bind_all ──────────────────────────────────────────── */
/*
 * Parse `usbip list -l`, extract every busid (format N-N), bind each.
 */
static void cmd_bind_all(int sock)
{
    char list_out[OUT_BUF_SIZE];
    run_cmd("usbip list -l", list_out, sizeof(list_out));

    char result[OUT_BUF_SIZE] = "";
    int  found = 0;

    char *ptr = list_out;
    while (*ptr) {
        /* Isolate one line */
        char *nl = strchr(ptr, '\n');
        size_t linelen = nl ? (size_t)(nl - ptr) : strlen(ptr);
        char line[512];
        if (linelen >= sizeof(line)) linelen = sizeof(line) - 1;
        memcpy(line, ptr, linelen);
        line[linelen] = '\0';
        ptr += linelen + (nl ? 1 : 0);

        /* Look for "busid " marker */
        const char *marker = strstr(line, "busid ");
        if (!marker) continue;
        marker += 6;

        /* Extract busid token (ends at space or '(') */
        char busid[32];
        size_t bidlen = 0;
        while (*marker && *marker != ' ' && *marker != '(' &&
               bidlen < sizeof(busid) - 1)
            busid[bidlen++] = *marker++;
        busid[bidlen] = '\0';
        if (bidlen == 0) continue;

        /* Must contain a '-' to be a valid busid */
        if (!strchr(busid, '-')) continue;

        /* Bind */
        char bind_cmd[128];
        snprintf(bind_cmd, sizeof(bind_cmd), "sudo usbip bind -b %s", busid);

        char bind_out[4096];
        int  rc = run_cmd(bind_cmd, bind_out, sizeof(bind_out));

        char entry[4096 + 128];
        snprintf(entry, sizeof(entry),
                 ">>> %s  (exit=%d)\n%s\n", bind_cmd, rc, bind_out);
        buf_append(result, sizeof(result), entry);
        found++;
    }

    if (found == 0)
        buf_append(result, sizeof(result), "No USB devices found to bind.\n");

    send_response(sock, result);
}

/* ── Command: bind_<busid> ──────────────────────────────────────── */
static void cmd_bind_busid(int sock, const char *busid)
{
    /* Allow only digits, hyphens, and dots */
    for (const char *p = busid; *p; p++) {
        if (!isdigit((unsigned char)*p) && *p != '-' && *p != '.') {
            send_response(sock, "ERROR: Invalid bus ID format.\n");
            return;
        }
    }

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "sudo usbip bind -b %s", busid);

    char out[OUT_BUF_SIZE];
    int  rc = run_cmd(cmd, out, sizeof(out));

    char result[OUT_BUF_SIZE];
    snprintf(result, sizeof(result), ">>> %s  (exit=%d)\n%s", cmd, rc, out);
    send_response(sock, result);
}

/* ── Command: bind_list ─────────────────────────────────────────── */
static void cmd_bind_list(int sock)
{
    char out[OUT_BUF_SIZE];
    int  rc = run_cmd("usbip port", out, sizeof(out));

    char result[OUT_BUF_SIZE];
    snprintf(result, sizeof(result), ">>> usbip port  (exit=%d)\n%s", rc, out);
    send_response(sock, result);
}

/* ── Command: pro_bind ──────────────────────────────────────────── */
/*
 * All-in-one server-side sequence:
 *   1. usbserver_init  (load modules + start usbipd)
 *   2. list_usb        (show available USB devices)
 *   3. bind_all        (bind every device for export)
 *
 * All output is collected into one response terminated by a single
 * END_OF_RESPONSE sentinel.
 */
static void cmd_pro_bind(int sock)
{
    char *result = (char *)calloc(1, OUT_BUF_SIZE);
    if (!result) {
        send_response(sock, "ERROR: out of memory\n");
        return;
    }

    char piece[OUT_BUF_SIZE / 2];
    char line[256];
    int  rc;

    /* ═══ STEP 1: usbserver_init ══════════════════════════════════ */
    buf_append(result, OUT_BUF_SIZE,
               "=== STEP 1 : usbserver_init ===\n\n");

    rc = run_cmd("sudo modprobe usbip_core", piece, sizeof(piece));
    snprintf(line, sizeof(line), ">>> sudo modprobe usbip_core  (exit=%d)\n", rc);
    buf_append(result, OUT_BUF_SIZE, line);
    if (piece[0]) {
        buf_append(result, OUT_BUF_SIZE, piece);
        if (result[strlen(result) - 1] != '\n')
            buf_append(result, OUT_BUF_SIZE, "\n");
    }
    buf_append(result, OUT_BUF_SIZE, "\n");

    rc = run_cmd("sudo modprobe usbip_host", piece, sizeof(piece));
    snprintf(line, sizeof(line), ">>> sudo modprobe usbip_host  (exit=%d)\n", rc);
    buf_append(result, OUT_BUF_SIZE, line);
    if (piece[0]) {
        buf_append(result, OUT_BUF_SIZE, piece);
        if (result[strlen(result) - 1] != '\n')
            buf_append(result, OUT_BUF_SIZE, "\n");
    }
    buf_append(result, OUT_BUF_SIZE, "\n");

    buf_append(result, OUT_BUF_SIZE,
               ">>> sudo pkill -x usbipd  (cleanup old instance)\n");
    (void)system("sudo pkill -x usbipd 2>/dev/null; sleep 0.3");

    buf_append(result, OUT_BUF_SIZE,
               ">>> sudo usbipd -D  (launching daemon in background)\n");
    rc = system("sudo usbipd -D &");
    snprintf(line, sizeof(line), "    system() returned: %d\n", rc);
    buf_append(result, OUT_BUF_SIZE, line);
    sleep(1);

    rc = run_cmd("pgrep -x usbipd", piece, sizeof(piece));
    if (rc == 0 && piece[0]) {
        char pid[32] = "";
        sscanf(piece, "%31s", pid);
        snprintf(line, sizeof(line), "    usbipd running (PID %s)\n\n", pid);
    } else {
        snprintf(line, sizeof(line),
                 "    WARNING: usbipd not running!\n"
                 "    Try: sudo usbipd -D\n\n");
    }
    buf_append(result, OUT_BUF_SIZE, line);

    /* ═══ STEP 2: list_usb ════════════════════════════════════════ */
    buf_append(result, OUT_BUF_SIZE,
               "=== STEP 2 : list_usb ===\n\n");

    rc = run_cmd("usbip list -l", piece, sizeof(piece));
    snprintf(line, sizeof(line), ">>> usbip list -l  (exit=%d)\n", rc);
    buf_append(result, OUT_BUF_SIZE, line);
    buf_append(result, OUT_BUF_SIZE, piece);
    if (piece[0] && result[strlen(result) - 1] != '\n')
        buf_append(result, OUT_BUF_SIZE, "\n");
    buf_append(result, OUT_BUF_SIZE, "\n");

    /* ═══ STEP 3: bind_all ════════════════════════════════════════ */
    buf_append(result, OUT_BUF_SIZE,
               "=== STEP 3 : bind_all ===\n\n");

    char list_out[OUT_BUF_SIZE / 2];
    run_cmd("usbip list -l", list_out, sizeof(list_out));
    int found = 0;

    char *ptr = list_out;
    while (*ptr) {
        char *nl = strchr(ptr, '\n');
        size_t linelen = nl ? (size_t)(nl - ptr) : strlen(ptr);
        char lbuf[512];
        if (linelen >= sizeof(lbuf)) linelen = sizeof(lbuf) - 1;
        memcpy(lbuf, ptr, linelen);
        lbuf[linelen] = '\0';
        ptr += linelen + (nl ? 1 : 0);

        const char *marker = strstr(lbuf, "busid ");
        if (!marker) continue;
        marker += 6;

        char busid[32];
        size_t bidlen = 0;
        while (*marker && *marker != ' ' && *marker != '(' &&
               bidlen < sizeof(busid) - 1)
            busid[bidlen++] = *marker++;
        busid[bidlen] = '\0';
        if (bidlen == 0 || !strchr(busid, '-')) continue;

        char bind_cmd[128];
        snprintf(bind_cmd, sizeof(bind_cmd), "sudo usbip bind -b %s", busid);
        char bind_out[2048];
        rc = run_cmd(bind_cmd, bind_out, sizeof(bind_out));

        char entry[2048 + 128];
        snprintf(entry, sizeof(entry),
                 ">>> %s  (exit=%d)\n%s\n", bind_cmd, rc, bind_out);
        buf_append(result, OUT_BUF_SIZE, entry);
        found++;
    }

    if (found == 0)
        buf_append(result, OUT_BUF_SIZE, "No USB devices found to bind.\n");

    buf_append(result, OUT_BUF_SIZE,
               "\n[pro_bind] Server-side complete.\n");

    send_response(sock, result);
    free(result);
}

/* ── Command dispatcher ─────────────────────────────────────────── */
static void dispatch(int sock, const char *cmd)
{
    printf("[server] Command: '%s'\n", cmd);

    if      (strcmp(cmd, "usbserver_init") == 0) cmd_usbserver_init(sock);
    else if (strcmp(cmd, "list_usb")       == 0) cmd_list_usb(sock);
    else if (strcmp(cmd, "bind_all")       == 0) cmd_bind_all(sock);
    else if (strcmp(cmd, "bind_list")      == 0) cmd_bind_list(sock);
    else if (strcmp(cmd, "pro_bind")       == 0) cmd_pro_bind(sock);
    else if (strncmp(cmd, "bind_", 5)      == 0) {
        const char *busid = cmd + 5;
        if (*busid == '\0')
            send_response(sock, "ERROR: Missing bus ID after 'bind_'.\n");
        else
            cmd_bind_busid(sock, busid);
    } else {
        char err[CMD_BUF_SIZE + 128];
        snprintf(err, sizeof(err),
                 "ERROR: Unknown command '%s'.\n"
                 "Valid: usbserver_init, list_usb, bind_all, "
                 "bind_list, bind_<busid>, pro_bind\n", cmd);
        send_response(sock, err);
    }
}

/* ── Client session ─────────────────────────────────────────────── */
static void handle_client(int client_fd, struct sockaddr_in *addr)
{
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip));
    printf("[server] Client connected: %s:%d\n", ip, ntohs(addr->sin_port));

    char buf[CMD_BUF_SIZE];
    int  pos = 0;

    while (1) {
        char    c;
        ssize_t n = recv(client_fd, &c, 1, 0);
        if (n <= 0) break;

        if (c == '\n' || c == '\r') {
            if (pos == 0) continue;
            buf[pos] = '\0';
            rtrim(buf);
            if (buf[0]) dispatch(client_fd, buf);
            pos = 0;
        } else if (pos < CMD_BUF_SIZE - 1) {
            buf[pos++] = c;
        }
    }

    printf("[server] Client disconnected: %s:%d\n", ip, ntohs(addr->sin_port));
    close(client_fd);
}

/* ── main ───────────────────────────────────────────────────────── */
int main(void)
{
    /* Ignore SIGPIPE – broken client must not kill the server */
    signal(SIGPIPE, SIG_IGN);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    /*
     * SO_REUSEADDR  – allows bind() immediately after crash/restart
     * SO_REUSEPORT  – also releases the port if the process was killed
     *                 while a client was still connected (Linux ≥ 3.9)
     * Together these eliminate the "address already in use" error that
     * occurs after a sudden power loss / forced restart.
     */
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port        = htons(SERVER_PORT)
    };

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, BACKLOG) < 0) {
        perror("listen");
        close(server_fd);
        return 1;
    }

    printf("[server] USB/IP server listening on port %d\n", SERVER_PORT);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int client_fd = accept(server_fd,
                               (struct sockaddr *)&client_addr,
                               &addr_len);
        if (client_fd < 0) { perror("accept"); continue; }

        handle_client(client_fd, &client_addr);
    }

    close(server_fd);
    return 0;
}