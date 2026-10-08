/* log.c - access log in Common Log Format */

#include "simplehttpd.h"

static int log_fd = STDOUT_FILENO;

/* opens the log file for appending, or uses stdout when path is NULL; returns 0 or -1 */
int log_open(const char *path)
{
    if (path == NULL) {
        log_fd = STDOUT_FILENO;
        return 0;
    }

    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0)
        return -1;

    log_fd = fd;
    return 0;
}

/* writes one log line with a single write(), so lines from different children never mix:
 * 127.0.0.1 - - [08/Oct/2026:14:03:22 +0100] "GET / HTTP/1.1" 200 1234 */
void log_request(const struct log_entry *entry)
{
    char      when[64];
    char      request[sizeof(entry->request_line)];
    char      line[1024];
    time_t    now = time(NULL);
    struct tm tm;

    localtime_r(&now, &tm);
    strftime(when, sizeof(when), "%d/%b/%Y:%H:%M:%S %z", &tm);

    size_t i;
    for (i = 0; entry->request_line[i] != '\0' && i < sizeof(request) - 1; i++) {
        unsigned char c = (unsigned char)entry->request_line[i];
        request[i] = (isprint(c) && c != '"' && c != '\\') ? (char)c : '?';
    }
    request[i] = '\0';

    int n = snprintf(line, sizeof(line), "%s - - [%s] \"%s\" %d %lld\n",
                     entry->client_ip, when, request, entry->status, entry->bytes);
    if (n >= (int)sizeof(line))
        n = sizeof(line) - 1;

    if (write(log_fd, line, n) < 0)
        perror("simplehttpd: log");
}
