/*
    server.c
    Linux TCP USB/IP Server
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define SERVER_PORT 5000
#define BUFFER_SIZE 4096
#define RESPONSE_SIZE 65536

static void append_text(char *dest, size_t dest_size, const char *src)
{
    size_t len = strlen(dest);

    if (len >= dest_size - 1)
        return;

    strncat(dest, src, dest_size - len - 1);
}

static void execute_command(const char *cmd,
                            char *response,
                            size_t response_size)
{
    FILE *fp;
    char line[1024];

    append_text(response, response_size, "CMD: ");
    append_text(response, response_size, cmd);
    append_text(response, response_size, "\n");

    fp = popen(cmd, "r");

    if (!fp)
    {
        append_text(response, response_size,
                    "ERROR: Failed to execute command\n");
        return;
    }

    while (fgets(line, sizeof(line), fp))
    {
        append_text(response, response_size, line);
    }

    int status = pclose(fp);

    char status_buf[128];

    snprintf(status_buf,
             sizeof(status_buf),
             "\nExit Status: %d\n\n",
             status);

    append_text(response, response_size, status_buf);
}

static void usbserver_init(char *response, size_t response_size)
{
    execute_command(
        "sudo modprobe usbip_core 2>&1",
        response,
        response_size);

    execute_command(
        "sudo modprobe usbip_host 2>&1",
        response,
        response_size);

    execute_command(
        "sudo usbipd -D 2>&1",
        response,
        response_size);
}

static void list_usb(char *response, size_t response_size)
{
    execute_command(
        "usbip list -l 2>&1",
        response,
        response_size);
}

static void bind_list(char *response, size_t response_size)
{
    execute_command(
        "usbip port 2>&1",
        response,
        response_size);
}

static void bind_single(const char *busid,
                        char *response,
                        size_t response_size)
{
    char cmd[256];

    snprintf(cmd,
             sizeof(cmd),
             "sudo usbip bind -b %s 2>&1",
             busid);

    execute_command(cmd, response, response_size);
}

static void bind_all(char *response, size_t response_size)
{
    FILE *fp;
    char line[1024];

    fp = popen("usbip list -l 2>&1", "r");

    if (!fp)
    {
        append_text(response,
                    response_size,
                    "ERROR: Cannot run usbip list -l\n");
        return;
    }

    while (fgets(line, sizeof(line), fp))
    {
        /*
            Typical line:

            - busid 1-1 (xxxx:xxxx)
        */

        char *p = strstr(line, "busid ");

        if (p)
        {
            char busid[64] = {0};

            p += 6;

            sscanf(p, "%63s", busid);

            char cmd[256];

            snprintf(cmd,
                     sizeof(cmd),
                     "sudo usbip bind -b %s 2>&1",
                     busid);

            execute_command(cmd,
                            response,
                            response_size);
        }
    }

    pclose(fp);
}

static void process_command(const char *cmd,
                            char *response,
                            size_t response_size)
{
    response[0] = '\0';

    if (strcmp(cmd, "usbserver_init") == 0)
    {
        usbserver_init(response, response_size);
    }
    else if (strcmp(cmd, "list_usb") == 0)
    {
        list_usb(response, response_size);
    }
    else if (strcmp(cmd, "bind_all") == 0)
    {
        bind_all(response, response_size);
    }
    else if (strcmp(cmd, "bind_list") == 0)
    {
        bind_list(response, response_size);
    }
    else if (strncmp(cmd, "bind_", 5) == 0)
    {
        bind_single(cmd + 5,
                    response,
                    response_size);
    }
    else
    {
        snprintf(response,
                 response_size,
                 "ERROR: Invalid command: %s\n",
                 cmd);
    }
}

int main(void)
{
    int server_fd;
    struct sockaddr_in server_addr;

    server_fd = socket(AF_INET,
                       SOCK_STREAM,
                       0);

    if (server_fd < 0)
    {
        perror("socket");
        return 1;
    }

    int opt = 1;

    setsockopt(server_fd,
               SOL_SOCKET,
               SO_REUSEADDR,
               &opt,
               sizeof(opt));

    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_fd,
             (struct sockaddr *)&server_addr,
             sizeof(server_addr)) < 0)
    {
        perror("bind");
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, 5) < 0)
    {
        perror("listen");
        close(server_fd);
        return 1;
    }

    printf("USB/IP Server listening on port %d\n",
           SERVER_PORT);

    while (1)
    {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(
            server_fd,
            (struct sockaddr *)&client_addr,
            &client_len);

        if (client_fd < 0)
        {
            perror("accept");
            continue;
        }

        printf("Client connected: %s\n",
               inet_ntoa(client_addr.sin_addr));

        char cmd[1024];
        int received;

        while ((received =
                    recv(client_fd,
                         cmd,
                         sizeof(cmd) - 1,
                         0)) > 0)
        {
            cmd[received] = '\0';

            char *newline = strchr(cmd, '\n');

            if (newline)
                *newline = '\0';

            printf("Command: %s\n", cmd);

            char response[RESPONSE_SIZE];

            process_command(
                cmd,
                response,
                sizeof(response));

            send(client_fd,
                 response,
                 strlen(response),
                 0);
        }

        close(client_fd);

        printf("Client disconnected\n");
    }

    close(server_fd);

    return 0;
}