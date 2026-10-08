/* simplehttpd.c - options, signals, sockets, and the fork-per-connection loop */

#include "simplehttpd.h"

static volatile sig_atomic_t stop_requested = 0;

/* SIGCHLD: reaps every finished child so none are left as zombies */
static void on_child(int sig)
{
    (void)sig;
    int saved_errno = errno;

    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;

    errno = saved_errno;
}

/* SIGINT / SIGTERM: asks the accept loop to stop */
static void on_stop(int sig)
{
    (void)sig;
    stop_requested = 1;
}

/* installs a signal handler with the given flags */
static void set_handler(int sig, void (*handler)(int), int flags)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sa.sa_flags   = flags;
    sigemptyset(&sa.sa_mask);

    if (sigaction(sig, &sa, NULL) != 0)
        perror("simplehttpd: sigaction");
}

/* prints the usage message */
static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [-p port] [-d root] [-l logfile] [-t seconds] [-h]\n"
            "  -p port      port to listen on (default %d)\n"
            "  -d root      folder to serve (default %s)\n"
            "  -l logfile   append the access log to this file (default stdout)\n"
            "  -t seconds   time a client has to send its request (default %d)\n"
            "  -h           show this message\n",
            prog, DEFAULT_PORT, DEFAULT_ROOT, DEFAULT_TIMEOUT);
}

/* converts text to an int in [min, max]; returns 0 or -1 */
static int parse_int(const char *text, int min, int max, int *out)
{
    char *end;
    errno = 0;
    long value = strtol(text, &end, 10);

    if (errno != 0 || end == text || *end != '\0' || value < min || value > max)
        return -1;

    *out = (int)value;
    return 0;
}

/* fills cfg from the command line; returns 0, 1 for -h, or -1 on error */
static int parse_options(int argc, char *argv[], struct server_config *cfg)
{
    const char *root = DEFAULT_ROOT;
    int         opt;

    cfg->port     = DEFAULT_PORT;
    cfg->timeout  = DEFAULT_TIMEOUT;
    cfg->log_path = NULL;

    while ((opt = getopt(argc, argv, "p:d:l:t:h")) != -1) {
        switch (opt) {
        case 'p':
            if (parse_int(optarg, 1, 65535, &cfg->port) != 0) {
                fprintf(stderr, "simplehttpd: invalid port '%s'\n", optarg);
                return -1;
            }
            break;
        case 't':
            if (parse_int(optarg, 1, 3600, &cfg->timeout) != 0) {
                fprintf(stderr, "simplehttpd: invalid timeout '%s'\n", optarg);
                return -1;
            }
            break;
        case 'd':
            root = optarg;
            break;
        case 'l':
            cfg->log_path = optarg;
            break;
        case 'h':
            usage(argv[0]);
            return 1;
        default:
            usage(argv[0]);
            return -1;
        }
    }

    if (optind < argc) {
        usage(argv[0]);
        return -1;
    }

    struct stat st;
    if (realpath(root, cfg->root) == NULL || stat(cfg->root, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "simplehttpd: '%s' is not a directory\n", root);
        return -1;
    }

    return 0;
}

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

/* sets the send timeout on a socket, so a client that stops reading cannot hold a child forever */
static void set_send_timeout(int fd, int seconds)
{
    struct timeval tv = { .tv_sec = seconds, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

/* stops sending and reads what the client is still sending, so closing the
 * socket does not reset the connection before the client reads the response */
static void drain_and_close(int fd)
{
    char            junk[4096];
    size_t          total = 0;
    struct timespec deadline;

    shutdown(fd, SHUT_WR);
    deadline_in(&deadline, 1);

    ssize_t n;
    while (total < 1024 * 1024 && set_recv_deadline(fd, &deadline) == 0 &&
           (n = recv(fd, junk, sizeof(junk), 0)) > 0)
        total += n;

    close(fd);
}

/* copies the first line of a request (up to CRLF) for the access log */
static void copy_request_line(const char *buf, struct log_entry *entry)
{
    size_t n = strcspn(buf, "\r\n");
    if (n >= sizeof(entry->request_line))
        n = sizeof(entry->request_line) - 1;

    memcpy(entry->request_line, buf, n);
    entry->request_line[n] = '\0';
}

/* reads one request, answers it, and logs it; runs in the child process */
static void handle_client(int fd, const struct server_config *cfg, const char *client_ip)
{
    char             buf[REQUEST_BUFSIZE];
    size_t           len;
    struct request   req;
    struct log_entry entry;

    memset(&entry, 0, sizeof(entry));
    snprintf(entry.client_ip, sizeof(entry.client_ip), "%s", client_ip);
    set_send_timeout(fd, cfg->timeout);

    int code = read_request(fd, buf, sizeof(buf), &len, cfg->timeout);
    if (code < 0) {
        close(fd);
        return;
    }

    copy_request_line(buf, &entry);
    if (entry.request_line[0] == '\0')
        snprintf(entry.request_line, sizeof(entry.request_line), "-");

    if (code == 0)
        code = parse_request(buf, &req);

    if (code == 0)
        serve_path(fd, cfg, &req, &entry);
    else
        send_error(fd, code, 0, &entry);

    log_request(&entry);
    drain_and_close(fd);
}

/* accepts connections until SIGINT/SIGTERM, forking a child for each one */
static void accept_loop(int listen_fd, const struct server_config *cfg)
{
    while (!stop_requested) {
        struct sockaddr_in client;
        socklen_t          len = sizeof(client);

        int client_fd = accept(listen_fd, (struct sockaddr *)&client, &len);
        if (client_fd < 0) {
            if (errno != EINTR)
                perror("simplehttpd: accept");
            continue;
        }

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client.sin_addr, ip, sizeof(ip));

        pid_t pid = fork();
        if (pid < 0) {
            perror("simplehttpd: fork");
            send_error(client_fd, 500, 0, NULL);
            close(client_fd);
        } else if (pid == 0) {
            close(listen_fd);
            set_handler(SIGINT, SIG_DFL, 0);
            set_handler(SIGTERM, SIG_DFL, 0);
            set_handler(SIGCHLD, SIG_DFL, 0);
            handle_client(client_fd, cfg, ip);
            _exit(EXIT_SUCCESS);
        } else {
            close(client_fd);
        }
    }
}

/* waits for every child that is still serving a request */
static void wait_for_children(void)
{
    while (1) {
        pid_t pid = waitpid(-1, NULL, 0);
        if (pid > 0 || (pid < 0 && errno == EINTR))
            continue;
        break;
    }
}

/* entry point - parses options, starts listening, and serves until stopped */
int main(int argc, char *argv[])
{
    struct server_config cfg;

    int rc = parse_options(argc, argv, &cfg);
    if (rc != 0)
        return rc > 0 ? EXIT_SUCCESS : EXIT_FAILURE;

    if (log_open(cfg.log_path) != 0) {
        perror("simplehttpd: cannot open log file");
        return EXIT_FAILURE;
    }

    set_handler(SIGCHLD, on_child, SA_RESTART | SA_NOCLDSTOP);
    set_handler(SIGINT, on_stop, 0);
    set_handler(SIGTERM, on_stop, 0);
    set_handler(SIGPIPE, SIG_IGN, 0);

    int listen_fd = open_listener(cfg.port);
    if (listen_fd < 0)
        return EXIT_FAILURE;

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("simplehttpd: serving '%s' on http://localhost:%d/ (pid %d)\n",
           cfg.root, cfg.port, (int)getpid());

    accept_loop(listen_fd, &cfg);

    close(listen_fd);
    wait_for_children();
    printf("simplehttpd: shutting down\n");
    return EXIT_SUCCESS;
}
