/* http.c - request parsing, responses, and file sending */

#include "simplehttpd.h"

/* maps file extensions to MIME types */
static const struct {
    const char *ext;
    const char *type;
} mime_table[] = {
    { "html", "text/html; charset=utf-8" },
    { "htm",  "text/html; charset=utf-8" },
    { "css",  "text/css; charset=utf-8" },
    { "js",   "text/javascript; charset=utf-8" },
    { "txt",  "text/plain; charset=utf-8" },
    { "png",  "image/png" },
    { "jpg",  "image/jpeg" },
    { "jpeg", "image/jpeg" },
    { "gif",  "image/gif" },
    { "ico",  "image/x-icon" },
    { NULL,   NULL }
};

/* returns the MIME type for a path based on its extension */
const char *mime_type(const char *path)
{
    const char *dot   = strrchr(path, '.');
    const char *slash = strrchr(path, '/');

    if (dot != NULL && (slash == NULL || dot > slash)) {
        for (int i = 0; mime_table[i].ext != NULL; i++) {
            if (strcasecmp(dot + 1, mime_table[i].ext) == 0)
                return mime_table[i].type;
        }
    }

    return "application/octet-stream";
}

/* returns the reason phrase for a status code */
const char *status_text(int code)
{
    switch (code) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    default:  return "Internal Server Error";
    }
}

/* writes the whole buffer, retrying after partial writes, returns 0 or -1 */
static int write_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        buf += n;
        len -= n;
    }
    return 0;
}

/* sends the status line and headers */
static void send_headers(int fd, int code, const char *type, long long length)
{
    char header[512];

    int n = snprintf(header, sizeof(header),
                     "HTTP/1.1 %d %s\r\n"
                     "Server: simplehttpd/1.0\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %lld\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     code, status_text(code), type, length);

    write_all(fd, header, n);
}

/* sends a small HTML error page */
static void send_error(int fd, int code)
{
    char body[256];

    int n = snprintf(body, sizeof(body),
                     "<!DOCTYPE html>\n<html><head><title>%d %s</title></head>"
                     "<body><h1>%d %s</h1></body></html>\n",
                     code, status_text(code), code, status_text(code));

    send_headers(fd, code, "text/html; charset=utf-8", n);
    write_all(fd, body, n);
}

/* reads until the blank line that ends the headers, returns 0 on success or -1 */
int read_request(int fd, char *buf, size_t size)
{
    size_t len = 0;

    while (len < size - 1) {
        ssize_t n = recv(fd, buf + len, size - 1 - len, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;

        len += n;
        buf[len] = '\0';

        if (strstr(buf, "\r\n\r\n") != NULL)
            return 0;
    }

    return -1;
}

/* splits the request line into method, target, and version, returns 0 or -1 */
int parse_request_line(char *buf, char **method, char **target, char **version)
{
    char *end = strstr(buf, "\r\n");
    if (end == NULL)
        return -1;
    *end = '\0';

    char *save;
    *method  = strtok_r(buf, " ", &save);
    *target  = strtok_r(NULL, " ", &save);
    *version = strtok_r(NULL, " ", &save);

    if (*method == NULL || *target == NULL || *version == NULL)
        return -1;
    if (strtok_r(NULL, " ", &save) != NULL)
        return -1;
    if (strncmp(*version, "HTTP/", 5) != 0 || (*target)[0] != '/')
        return -1;

    return 0;
}

/* sends a regular file, returns the status code that was sent */
static int send_file(int fd, const char *path)
{
    int file_fd = open(path, O_RDONLY);
    if (file_fd < 0) {
        int code = (errno == ENOENT || errno == ENOTDIR) ? 404 :
                   (errno == EACCES) ? 403 : 500;
        send_error(fd, code);
        return code;
    }

    struct stat st;
    if (fstat(file_fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(file_fd);
        send_error(fd, 403);
        return 403;
    }

    send_headers(fd, 200, mime_type(path), (long long)st.st_size);

    char    chunk[FILE_BUFSIZE];
    ssize_t n;
    while ((n = read(file_fd, chunk, sizeof(chunk))) > 0) {
        if (write_all(fd, chunk, n) != 0)
            break;
    }

    close(file_fd);
    return 200;
}

/* reads one request from the client, answers it, and prints a log line */
void handle_client(int fd, const char *root, const char *client_ip)
{
    char buf[REQUEST_BUFSIZE];

    if (read_request(fd, buf, sizeof(buf)) != 0) {
        send_error(fd, 400);
        printf("%s \"-\" 400\n", client_ip);
        return;
    }

    char *method, *target, *version;
    if (parse_request_line(buf, &method, &target, &version) != 0) {
        send_error(fd, 400);
        printf("%s \"-\" 400\n", client_ip);
        return;
    }

    int code;
    char *query = strchr(target, '?');
    if (query != NULL)
        *query = '\0';

    if (strcmp(method, "GET") != 0) {
        code = 405;
        send_error(fd, code);
    } else if (strstr(target, "..") != NULL) {
        code = 403;
        send_error(fd, code);
    } else {
        char path[PATH_MAX];
        const char *file = strcmp(target, "/") == 0 ? "/index.html" : target;

        snprintf(path, sizeof(path), "%s%s", root, file);
        code = send_file(fd, path);
    }

    printf("%s \"%s %s %s\" %d\n", client_ip, method, target, version, code);
}
