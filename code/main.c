/*
 * server.c - USB/IP TCP Server for Linux (Raspberry Pi)
 *
 * Build:
 *   gcc -Wall -Wextra -o usbip_server server.c
 *
 * Run (root required for usbip commands):
 *   sudo ./usbip_server
 *
 * The server listens on TCP port 5000 and accepts text commands from clients.
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

/* ── Configuration ─────────────────────────────────────────────── */
#define SERVER_PORT      5000
#define BACKLOG          5
#define CMD_BUF_SIZE     256
#define OUT_BUF_SIZE     65536   /* 64 KiB – command output buffer   */
#define SEND_CHUNK       4096

/* ── Helpers ────────────────────────────────────────────────────── */

/* Run a shell command and capture stdout + stderr into buf.
 * Returns the exit code of the command, or -1 on popen failure.
 */
static int run_cmd(const char *cmd, char *buf, size_t bufsz)
{
    FILE *fp;
    size_t total = 0;
    char tmp[512];

    /* Redirect stderr to stdout so we capture error messages too */
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

/* Send all bytes in buf over a socket (handles partial writes). */
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

/* Send a NUL-terminated string followed by the sentinel "END_OF_RESPONSE\n"
 * so the client knows when the reply is complete.
 */
static void send_response(int sock, const char *msg)
{
    send_all(sock, msg, strlen(msg));
    send_all(sock, "END_OF_RESPONSE\n", 16);
}

/* Strip trailing whitespace / newline from a string in-place. */
static void rtrim(char *s)
{
    int i = (int)strlen(s) - 1;
    while (i >= 0 && (s[i] == '\n' || s[i] == '\r' || s[i] == ' '))
        s[i--] = '\0';
}

/* ── Command handlers ───────────────────────────────────────────── */

/*
 * usbserver_init
 * Load kernel modules and start the USB/IP daemon.
 *
 * NOTE: usbipd -D forks itself into the background and never exits, so we
 * MUST NOT use popen() on it directly — that would block forever.
 * Instead we launch it detached with "sudo usbipd -D &" via system(), wait
 * 1 second for it to start, then verify it is running with pgrep.
 */
static void cmd_usbserver_init(int sock)
{
    char result[OUT_BUF_SIZE] = "";
    char piece[OUT_BUF_SIZE / 2];
    char line[256];
    int  rc;

    /* ── Step 1: modprobe usbip_core ─────────────────────────────── */
    rc = run_cmd("sudo modprobe usbip_core", piece, sizeof(piece));
    snprintf(line, sizeof(line), ">>> sudo modprobe usbip_core  (exit=%d)\n", rc);
    strncat(result, line,  sizeof(result) - strlen(result) - 1);
    if (piece[0]) {
        strncat(result, piece, sizeof(result) - strlen(result) - 1);
        if (result[strlen(result)-1] != '\n')
            strncat(result, "\n", sizeof(result) - strlen(result) - 1);
    }
    strncat(result, "\n", sizeof(result) - strlen(result) - 1);

    /* ── Step 2: modprobe usbip_host ─────────────────────────────── */
    rc = run_cmd("sudo modprobe usbip_host", piece, sizeof(piece));
    snprintf(line, sizeof(line), ">>> sudo modprobe usbip_host  (exit=%d)\n", rc);
    strncat(result, line,  sizeof(result) - strlen(result) - 1);
    if (piece[0]) {
        strncat(result, piece, sizeof(result) - strlen(result) - 1);
        if (result[strlen(result)-1] != '\n')
            strncat(result, "\n", sizeof(result) - strlen(result) - 1);
    }
    strncat(result, "\n", sizeof(result) - strlen(result) - 1);

    /* ── Step 3: usbipd -D (daemon — must NOT block) ─────────────── */
    /*
     * Kill any existing usbipd instance first so we don't double-start it.
     * Then launch detached.  system() returns immediately because the shell
     * sees "&" and does not wait for the child.
     */
    strncat(result, ">>> sudo pkill -x usbipd  (cleanup old instance)\n",
            sizeof(result) - strlen(result) - 1);
    system("sudo pkill -x usbipd 2>/dev/null; sleep 0.3");

    strncat(result, ">>> sudo usbipd -D  (launching daemon in background)\n",
            sizeof(result) - strlen(result) - 1);

    rc = system("sudo usbipd -D &");
    snprintf(line, sizeof(line), "    system() returned: %d\n", rc);
    strncat(result, line, sizeof(result) - strlen(result) - 1);

    /* Give the daemon a moment to start */
    sleep(1);

    /* ── Step 4: verify usbipd is actually running ───────────────── */
    rc = run_cmd("pgrep -x usbipd", piece, sizeof(piece));
    if (rc == 0 && piece[0]) {
        /* pgrep prints the PID */
        char pid[32] = "";
        sscanf(piece, "%31s", pid);
        snprintf(line, sizeof(line),
                 "    usbipd is running  (PID %s) ✓\n\n", pid);
    } else {
        snprintf(line, sizeof(line),
                 "    WARNING: usbipd does not appear to be running!\n"
                 "    Try running manually: sudo usbipd -D\n\n");
    }
    strncat(result, line, sizeof(result) - strlen(result) - 1);

    send_response(sock, result);
}

/*
 * list_usb
 * Show locally available USB devices.
 */
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

/*
 * bind_all
 * Parse `usbip list -l` output, find every busid, and bind each one.
 *
 * The relevant lines look like:
 *  - busid 1-1 (xxxx:yyyy)
 */
static void cmd_bind_all(int sock)
{
    char list_out[OUT_BUF_SIZE];
    run_cmd("usbip list -l", list_out, sizeof(list_out));

    char result[OUT_BUF_SIZE] = "";
    char line[512];
    int found = 0;

    /* Walk through the output line by line */
    char *ptr = list_out;
    while (*ptr) {
        /* Extract one line */
        char *nl = strchr(ptr, '\n');
        size_t linelen = nl ? (size_t)(nl - ptr) : strlen(ptr);
        if (linelen >= sizeof(line)) linelen = sizeof(line) - 1;
        memcpy(line, ptr, linelen);
        line[linelen] = '\0';
        ptr += linelen + (nl ? 1 : 0);

        /* Look for " - busid " */
        const char *marker = strstr(line, "busid ");
        if (!marker) continue;

        marker += 6; /* skip "busid " */

        /* The busid ends at the first space or '(' */
        char busid[32];
        size_t bidlen = 0;
        while (*marker && *marker != ' ' && *marker != '(' && bidlen < sizeof(busid)-1)
            busid[bidlen++] = *marker++;
        busid[bidlen] = '\0';

        if (bidlen == 0) continue;

        /* Validate: busid must match digits-digits pattern */
        int valid = 0;
        for (size_t j = 0; j < bidlen; j++) {
            if (busid[j] == '-') { valid = 1; break; }
        }
        if (!valid) continue;

        /* Bind this device */
        char bind_cmd[128];
        snprintf(bind_cmd, sizeof(bind_cmd), "sudo usbip bind -b %s", busid);

        char bind_out[4096];
        int rc = run_cmd(bind_cmd, bind_out, sizeof(bind_out));

        char entry[4096 + 128];
        snprintf(entry, sizeof(entry),
                 ">>> %s  (exit=%d)\n%s\n", bind_cmd, rc, bind_out);
        strncat(result, entry, sizeof(result) - strlen(result) - 1);
        found++;
    }

    if (found == 0) {
        strncat(result, "No USB devices found to bind.\n",
                sizeof(result) - strlen(result) - 1);
    }

    send_response(sock, result);
}

/*
 * bind_<busid>
 * Bind a specific USB device by bus ID.
 */
static void cmd_bind_busid(int sock, const char *busid)
{
    /* Basic sanity check on the bus ID (digits, hyphens, dots only) */
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

/*
 * bind_list
 * Show currently bound (exported) USB devices.
 */
static void cmd_bind_list(int sock)
{
    /* usbip port shows bound/attached devices */
    char out[OUT_BUF_SIZE];
    int rc = run_cmd("usbip port", out, sizeof(out));

    char result[OUT_BUF_SIZE];
    snprintf(result, sizeof(result), ">>> usbip port  (exit=%d)\n%s", rc, out);
    send_response(sock, result);
}

/* ── Command dispatcher ─────────────────────────────────────────── */

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
        /* bind_<busid> */
        const char *busid = cmd + 5;
        if (*busid == '\0') {
            send_response(sock, "ERROR: Missing bus ID after 'bind_'.\n");
        } else {
            cmd_bind_busid(sock, busid);
        }

    } else {
        char err[CMD_BUF_SIZE + 64];
        snprintf(err, sizeof(err),
                 "ERROR: Unknown command '%s'.\n"
                 "Valid commands: usbserver_init, list_usb, bind_all, "
                 "bind_list, bind_<busid>\n", cmd);
        send_response(sock, err);
    }
}

/* ── Client session handler ─────────────────────────────────────── */

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
        if (n <= 0) break;          /* disconnected or error */

        if (c == '\n' || c == '\r') {
            if (pos == 0) continue; /* skip blank lines      */
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

/* ── main ───────────────────────────────────────────────────────── */

int main(void)
{
    /* Ignore SIGPIPE so a broken client doesn't kill the server */
    signal(SIGPIPE, SIG_IGN);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    /* Allow quick restart after crash */
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(SERVER_PORT);

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
    printf("[server] Waiting for client connections...\n");

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int client_fd = accept(server_fd,
                               (struct sockaddr *)&client_addr,
                               &addr_len);
        if (client_fd < 0) {
            perror("accept");
            continue;
        }

        handle_client(client_fd, &client_addr);
    }

    close(server_fd);
    return 0;
}