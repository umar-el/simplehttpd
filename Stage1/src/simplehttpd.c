/* simplehttpd.c - socket setup and the main accept loop */

#include "simplehttpd.h"

/* creates a TCP socket listening on the given port, returns the fd or -1 */
static int open_listener(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("simplehttpd: socket");
        return -1;
    }

    int on = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) != 0)
        perror("simplehttpd: setsockopt");

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("simplehttpd: bind");
        close(fd);
        return -1;
    }

    if (listen(fd, LISTEN_BACKLOG) != 0) {
        perror("simplehttpd: listen");
        close(fd);
        return -1;
    }

    return fd;
}

/* entry point - serves one client at a time, forever */
int main(int argc, char *argv[])
{
    int         port = DEFAULT_PORT;
    const char *root = DEFAULT_ROOT;

    if (argc > 3) {
        fprintf(stderr, "Usage: %s [port] [root]\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (argc >= 2) {
        port = atoi(argv[1]);
        if (port <= 0 || port > 65535) {
            fprintf(stderr, "simplehttpd: invalid port '%s'\n", argv[1]);
            return EXIT_FAILURE;
        }
    }
    if (argc == 3)
        root = argv[2];

    struct stat st;
    if (stat(root, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "simplehttpd: '%s' is not a directory\n", root);
        return EXIT_FAILURE;
    }

    signal(SIGPIPE, SIG_IGN);

    int listen_fd = open_listener(port);
    if (listen_fd < 0)
        return EXIT_FAILURE;

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("simplehttpd: serving '%s' on http://localhost:%d/\n", root, port);

    while (1) {
        struct sockaddr_in client;
        socklen_t          len = sizeof(client);

        int client_fd = accept(listen_fd, (struct sockaddr *)&client, &len);
        if (client_fd < 0) {
            perror("simplehttpd: accept");
            continue;
        }

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client.sin_addr, ip, sizeof(ip));

        handle_client(client_fd, root, ip);
        close(client_fd);
    }

    return EXIT_SUCCESS;
}
