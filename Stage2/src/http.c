/* http.c - reading, parsing, and answering HTTP requests */

#include "simplehttpd.h"

/* sets deadline to the given number of seconds from now */
void deadline_in(struct timespec *deadline, int seconds)
{
    clock_gettime(CLOCK_MONOTONIC, deadline);
    deadline->tv_sec += seconds;
}

/* sets the socket's receive timeout to the time left before deadline;
 * returns 0, or -1 if the deadline has already passed */
int set_recv_deadline(int fd, const struct timespec *deadline)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    long long left_us = (deadline->tv_sec - now.tv_sec) * 1000000LL +
                        (deadline->tv_nsec - now.tv_nsec) / 1000;
    if (left_us <= 0)
        return -1;

    struct timeval tv = { .tv_sec = left_us / 1000000, .tv_usec = left_us % 1000000 };
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0 ? 0 : -1;
}

/* reads until the blank line that ends the headers, giving the client
 * timeout seconds in total. returns 0, 408 (timed out), 431 (too large),
 * 400 (closed mid-request), or -1 (closed before sending anything, or a socket error) */
int read_request(int fd, char *buf, size_t size, size_t *len, int timeout)
{
    struct timespec deadline;
    deadline_in(&deadline, timeout);

    *len   = 0;
    buf[0] = '\0';

    while (*len < size - 1) {
        if (set_recv_deadline(fd, &deadline) != 0)
            return 408;

        ssize_t n = recv(fd, buf + *len, size - 1 - *len, 0);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return 408;
            return -1;
        }
        if (n == 0)
            return *len == 0 ? -1 : 400;

        *len += n;
        buf[*len] = '\0';

        if (strstr(buf, "\r\n\r\n") != NULL)
            return 0;
    }

    return 431;
}

/* returns the value of a hex digit, or -1 */
static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* decodes %XX escapes; fails on bad escapes, %00, or if out is too small */
int url_decode(const char *in, char *out, size_t size)
{
    size_t j = 0;

    for (size_t i = 0; in[i] != '\0'; i++) {
        char c = in[i];

        if (c == '%') {
            int hi = hex_value(in[i + 1]);
            int lo = hi < 0 ? -1 : hex_value(in[i + 2]);
            if (hi < 0 || lo < 0)
                return -1;

            c = (char)(hi * 16 + lo);
            if (c == '\0')
                return -1;
            i += 2;
        }

        if (j + 1 >= size)
            return -1;
        out[j++] = c;
    }

    out[j] = '\0';
    return 0;
}

/* strips spaces and tabs from both ends of s, in place */
static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;

    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t'))
        *--end = '\0';

    return s;
}

/* splits the first line into method, target, and version; returns 0 or a status code */
static int parse_request_line(char *line, struct request *req)
{
    char *save;

    req->method  = strtok_r(line, " ", &save);
    req->target  = strtok_r(NULL, " ", &save);
    req->version = strtok_r(NULL, " ", &save);

    if (req->method == NULL || req->target == NULL || req->version == NULL)
        return 400;
    if (strtok_r(NULL, " ", &save) != NULL)
        return 400;

    const char *v = req->version;
    if (strncmp(v, "HTTP/", 5) != 0 || strlen(v) != 8 ||
        !isdigit((unsigned char)v[5]) || v[6] != '.' || !isdigit((unsigned char)v[7]))
        return 400;
    if (strcmp(v, "HTTP/1.0") != 0 && strcmp(v, "HTTP/1.1") != 0)
        return 505;

    if (strcmp(req->method, "GET") != 0 && strcmp(req->method, "HEAD") != 0)
        return 405;
    if (req->target[0] != '/')
        return 400;
    for (const char *c = req->target; *c != '\0'; c++) {
        if ((unsigned char)*c <= ' ' || (unsigned char)*c == 0x7f)
            return 400;
    }

    return 0;
}

/* parses "Name: value" lines until the blank line; returns 0 or 400 */
static int parse_headers(char *p, struct request *req)
{
    while (1) {
        char *end = strstr(p, "\r\n");
        if (end == NULL)
            return 400;
        *end = '\0';

        if (*p == '\0')
            return 0;

        char *colon = strchr(p, ':');
        if (colon == NULL || colon == p)
            return 400;
        *colon = '\0';

        if (strpbrk(p, " \t") != NULL)
            return 400;
        if (req->header_count == MAX_HEADERS)
            return 400;

        req->headers[req->header_count].name  = p;
        req->headers[req->header_count].value = trim(colon + 1);
        req->header_count++;

        p = end + 2;
    }
}

/* parses a complete request held in buf, which it modifies.
 * returns 0, or the status code to answer with */
int parse_request(char *buf, struct request *req)
{
    memset(req, 0, sizeof(*req));

    char *end = strstr(buf, "\r\n");
    if (end == NULL)
        return 400;
    *end = '\0';

    int code = parse_request_line(buf, req);
    if (code != 0)
        return code;

    code = parse_headers(end + 2, req);
    if (code != 0)
        return code;

    if (strcmp(req->version, "HTTP/1.1") == 0 && get_header(req, "Host") == NULL)
        return 400;

    char raw[PATH_MAX];
    size_t path_len = strcspn(req->target, "?#");
    if (path_len >= sizeof(raw))
        return 400;
    memcpy(raw, req->target, path_len);
    raw[path_len] = '\0';

    if (url_decode(raw, req->path, sizeof(req->path)) != 0)
        return 400;

    return 0;
}

/* returns a header's value (names are case-insensitive), or NULL */
const char *get_header(const struct request *req, const char *name)
{
    for (int i = 0; i < req->header_count; i++) {
        if (strcasecmp(req->headers[i].name, name) == 0)
            return req->headers[i].value;
    }
    return NULL;
}

/* returns the reason phrase for a status code */
const char *status_text(int code)
{
    switch (code) {
    case 200: return "OK";
    case 301: return "Moved Permanently";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 431: return "Request Header Fields Too Large";
    case 505: return "HTTP Version Not Supported";
    default:  return "Internal Server Error";
    }
}

/* writes the whole buffer, retrying after partial writes; returns len or -1 */
ssize_t write_all(int fd, const void *buf, size_t len)
{
    const char *p    = buf;
    size_t      left = len;

    while (left > 0) {
        ssize_t n = write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p    += n;
        left -= n;
    }

    return (ssize_t)len;
}

/* formats a time as an HTTP date, e.g. "Thu, 08 Oct 2026 13:03:22 GMT" */
static void http_date(time_t t, char *out, size_t size)
{
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, size, "%a, %d %b %Y %H:%M:%S GMT", &tm);
}

/* appends formatted text to buf at *len; returns 0, or -1 if it does not fit */
static int append(char *buf, size_t size, size_t *len, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static int append(char *buf, size_t size, size_t *len, const char *fmt, ...)
{
    if (*len >= size)
        return -1;

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *len, size - *len, fmt, ap);
    va_end(ap);

    if (n < 0 || (size_t)n >= size - *len)
        return -1;

    *len += n;
    return 0;
}

/* sends the status line and headers; returns the bytes written, or -1 if they
 * do not fit in the header buffer or cannot be written */
int send_headers(int fd, int code, const struct response_info *info)
{
    char   header[PATH_MAX + 1024];
    char   date[64];
    size_t n   = 0;
    int    bad = 0;

    http_date(time(NULL), date, sizeof(date));
    bad |= append(header, sizeof(header), &n, "HTTP/1.1 %d %s\r\nDate: %s\r\nServer: %s\r\n",
                  code, status_text(code), date, SERVER_NAME);

    if (info->content_type != NULL)
        bad |= append(header, sizeof(header), &n, "Content-Type: %s\r\n", info->content_type);
    if (info->content_length >= 0)
        bad |= append(header, sizeof(header), &n, "Content-Length: %lld\r\n", info->content_length);
    if (info->last_modified > 0) {
        http_date(info->last_modified, date, sizeof(date));
        bad |= append(header, sizeof(header), &n, "Last-Modified: %s\r\n", date);
    }
    if (info->location != NULL)
        bad |= append(header, sizeof(header), &n, "Location: %s\r\n", info->location);
    if (info->allow != NULL)
        bad |= append(header, sizeof(header), &n, "Allow: %s\r\n", info->allow);

    bad |= append(header, sizeof(header), &n, "Connection: close\r\n\r\n");

    if (bad)
        return -1;

    return (int)write_all(fd, header, n);
}

/* sends a small HTML error page and records the status for the log */
void send_error(int fd, int code, int head_only, struct log_entry *entry)
{
    char body[512];

    int n = snprintf(body, sizeof(body),
                     "<!DOCTYPE html>\n<html><head><title>%d %s</title></head>\n"
                     "<body><h1>%d %s</h1><hr><p>%s</p></body></html>\n",
                     code, status_text(code), code, status_text(code), SERVER_NAME);

    struct response_info info = {
        .content_type   = "text/html; charset=utf-8",
        .content_length = n,
        .allow          = code == 405 ? "GET, HEAD" : NULL,
    };

    send_headers(fd, code, &info);
    if (!head_only)
        write_all(fd, body, n);

    if (entry != NULL) {
        entry->status = code;
        entry->bytes  = head_only ? 0 : n;
    }
}
