/* simplehttpd.h - shared constants, includes, and function prototypes */

#ifndef SIMPLEHTTPD_H
#define SIMPLEHTTPD_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define REQUEST_BUFSIZE  8192
#define FILE_BUFSIZE     65536
#define DEFAULT_PORT     8080
#define DEFAULT_ROOT     "www"
#define LISTEN_BACKLOG   16

/* request handling */
int  read_request(int fd, char *buf, size_t size);
int  parse_request_line(char *buf, char **method, char **target, char **version);
void handle_client(int fd, const char *root, const char *client_ip);

/* helpers */
const char *mime_type(const char *path);
const char *status_text(int code);

#endif
