/* simplehttpd.h - shared constants, structures, and function prototypes */

#ifndef SIMPLEHTTPD_H
#define SIMPLEHTTPD_H

#define _GNU_SOURCE

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define REQUEST_BUFSIZE  8192
#define FILE_BUFSIZE     65536
#define MAX_HEADERS      32
#define DEFAULT_PORT     8080
#define DEFAULT_ROOT     "www"
#define DEFAULT_TIMEOUT  10
#define LISTEN_BACKLOG   64
#define SERVER_NAME      "simplehttpd/1.0"

/* one "Name: value" header, pointing into the request buffer */
struct header {
    char *name;
    char *value;
};

/* a parsed request; the char pointers point into the request buffer */
struct request {
    char         *method;
    char         *target;
    char         *version;
    char          path[PATH_MAX];
    struct header headers[MAX_HEADERS];
    int           header_count;
};

/* optional headers for a response; NULL / -1 / 0 leave a header out */
struct response_info {
    const char *content_type;
    long long   content_length;
    time_t      last_modified;
    const char *location;
    const char *allow;
};

/* what the access log records about one response */
struct log_entry {
    char      client_ip[INET_ADDRSTRLEN];
    char      request_line[512];
    int       status;
    long long bytes;
};

/* settings from the command line */
struct server_config {
    char        root[PATH_MAX];
    int         port;
    int         timeout;
    const char *log_path;
};

/* http.c - reading, parsing, and answering requests */
void        deadline_in(struct timespec *deadline, int seconds);
int         set_recv_deadline(int fd, const struct timespec *deadline);
int         read_request(int fd, char *buf, size_t size, size_t *len, int timeout);
int         parse_request(char *buf, struct request *req);
int         url_decode(const char *in, char *out, size_t size);
const char *get_header(const struct request *req, const char *name);
const char *status_text(int code);
ssize_t     write_all(int fd, const void *buf, size_t len);
int         send_headers(int fd, int code, const struct response_info *info);
void        send_error(int fd, int code, int head_only, struct log_entry *entry);

/* files.c - mapping URLs to files and sending them */
int         resolve_path(const char *root, const char *url_path, char *out, size_t size);
const char *mime_type(const char *path);
int         html_escape(const char *in, char *out, size_t size);
int         url_encode(const char *in, char *out, size_t size);
void        serve_path(int fd, const struct server_config *cfg, const struct request *req,
                       struct log_entry *entry);

/* log.c - access log */
int         log_open(const char *path);
void        log_request(const struct log_entry *entry);

#endif
