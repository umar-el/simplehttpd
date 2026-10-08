/* files.c - path resolution, MIME types, file sending, and directory listings */

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
    { "json", "application/json; charset=utf-8" },
    { "txt",  "text/plain; charset=utf-8" },
    { "md",   "text/markdown; charset=utf-8" },
    { "xml",  "application/xml; charset=utf-8" },
    { "png",  "image/png" },
    { "jpg",  "image/jpeg" },
    { "jpeg", "image/jpeg" },
    { "gif",  "image/gif" },
    { "svg",  "image/svg+xml" },
    { "ico",  "image/x-icon" },
    { "webp", "image/webp" },
    { "pdf",  "application/pdf" },
    { "wasm", "application/wasm" },
    { NULL,   NULL }
};

/* one row of a directory listing */
struct dir_item {
    char     *name;
    int       is_dir;
    long long size;
    time_t    mtime;
};

/* a growable string used to build listing pages */
struct strbuf {
    char  *data;
    size_t len;
    size_t cap;
    int    failed;
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

/* joins root and url_path and resolves symlinks and "..".
 * returns 0, 403 if the result is outside root, 404 if missing, or 500 */
int resolve_path(const char *root, const char *url_path, char *out, size_t size)
{
    char joined[PATH_MAX];
    char resolved[PATH_MAX];

    if (snprintf(joined, sizeof(joined), "%s%s", root, url_path) >= (int)sizeof(joined))
        return 404;

    if (realpath(joined, resolved) == NULL) {
        if (errno == ENOENT || errno == ENOTDIR || errno == ENAMETOOLONG || errno == ELOOP)
            return 404;
        if (errno == EACCES)
            return 403;
        return 500;
    }

    size_t root_len = strlen(root);
    if (strncmp(resolved, root, root_len) != 0 ||
        (resolved[root_len] != '\0' && resolved[root_len] != '/'))
        return 403;

    if (snprintf(out, size, "%s", resolved) >= (int)size)
        return 500;

    return 0;
}

/* replaces & < > " ' with HTML entities; returns 0, or -1 if out is too small */
int html_escape(const char *in, char *out, size_t size)
{
    size_t j = 0;

    for (size_t i = 0; in[i] != '\0'; i++) {
        const char *rep;
        char        single[2] = { in[i], '\0' };

        switch (in[i]) {
        case '&':  rep = "&amp;";  break;
        case '<':  rep = "&lt;";   break;
        case '>':  rep = "&gt;";   break;
        case '"':  rep = "&quot;"; break;
        case '\'': rep = "&#39;";  break;
        default:   rep = single;   break;
        }

        size_t n = strlen(rep);
        if (j + n >= size)
            return -1;
        memcpy(out + j, rep, n);
        j += n;
    }

    out[j] = '\0';
    return 0;
}

/* percent-encodes everything except A-Z a-z 0-9 - . _ ~ and /;
 * returns 0, or -1 if out is too small */
int url_encode(const char *in, char *out, size_t size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t j = 0;

    for (size_t i = 0; in[i] != '\0'; i++) {
        unsigned char c = (unsigned char)in[i];

        if (isalnum(c) || strchr("-._~/", c) != NULL) {
            if (j + 1 >= size)
                return -1;
            out[j++] = c;
        } else {
            if (j + 3 >= size)
                return -1;
            out[j++] = '%';
            out[j++] = hex[c >> 4];
            out[j++] = hex[c & 15];
        }
    }

    out[j] = '\0';
    return 0;
}

/* returns 1 if the If-Modified-Since header says the client's copy is current */
static int not_modified(const struct request *req, time_t mtime)
{
    const char *value = get_header(req, "If-Modified-Since");
    if (value == NULL)
        return 0;

    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    const char *end = strptime(value, "%a, %d %b %Y %H:%M:%S GMT", &tm);
    if (end == NULL || *end != '\0')
        return 0;

    return mtime <= timegm(&tm);
}

/* maps an errno from open/opendir to a status code */
static int errno_status(int err)
{
    if (err == ENOENT || err == ENOTDIR)
        return 404;
    if (err == EACCES)
        return 403;
    return 500;
}

/* sends a regular file (headers only for HEAD), honouring If-Modified-Since */
static void send_file(int fd, const char *path, const struct request *req,
                      int head_only, struct log_entry *entry)
{
    int file_fd = open(path, O_RDONLY);
    if (file_fd < 0) {
        send_error(fd, errno_status(errno), head_only, entry);
        return;
    }

    struct stat st;
    if (fstat(file_fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(file_fd);
        send_error(fd, 403, head_only, entry);
        return;
    }

    if (not_modified(req, st.st_mtime)) {
        struct response_info info = { .content_length = -1, .last_modified = st.st_mtime };
        send_headers(fd, 304, &info);
        entry->status = 304;
        entry->bytes  = 0;
        close(file_fd);
        return;
    }

    struct response_info info = {
        .content_type   = mime_type(path),
        .content_length = (long long)st.st_size,
        .last_modified  = st.st_mtime,
    };
    send_headers(fd, 200, &info);
    entry->status = 200;
    entry->bytes  = 0;

    if (!head_only) {
        char   *chunk = malloc(FILE_BUFSIZE);
        ssize_t n;

        while (chunk != NULL && (n = read(file_fd, chunk, FILE_BUFSIZE)) > 0) {
            if (write_all(fd, chunk, n) < 0)
                break;
            entry->bytes += n;
        }
        free(chunk);
    }

    close(file_fd);
}

/* appends formatted text to a strbuf, growing it as needed */
static void sb_printf(struct strbuf *sb, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void sb_printf(struct strbuf *sb, const char *fmt, ...)
{
    if (sb->failed)
        return;

    while (1) {
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(sb->data + sb->len, sb->cap - sb->len, fmt, ap);
        va_end(ap);

        if (n < 0) {
            sb->failed = 1;
            return;
        }
        if (sb->len + n < sb->cap) {
            sb->len += n;
            return;
        }

        size_t new_cap = (sb->cap + n + 1) * 2;
        char  *grown   = realloc(sb->data, new_cap);
        if (grown == NULL) {
            sb->failed = 1;
            return;
        }
        sb->data = grown;
        sb->cap  = new_cap;
    }
}

/* qsort comparator: directories first, then by name */
static int compare_items(const void *a, const void *b)
{
    const struct dir_item *x = a;
    const struct dir_item *y = b;

    if (x->is_dir != y->is_dir)
        return y->is_dir - x->is_dir;
    return strcmp(x->name, y->name);
}

/* reads a directory into a sorted array, skipping dotfiles; returns the count or -1 */
static int read_dir_items(const char *dir_path, struct dir_item **items_out)
{
    DIR *dir = opendir(dir_path);
    if (dir == NULL)
        return -1;

    struct dir_item *items = NULL;
    int              count = 0;
    int              cap   = 0;
    struct dirent   *ent;

    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.')
            continue;

        char        path[PATH_MAX];
        struct stat st;
        if (snprintf(path, sizeof(path), "%s/%s", dir_path, ent->d_name) >= (int)sizeof(path) ||
            stat(path, &st) != 0)
            continue;

        if (count == cap) {
            cap = cap == 0 ? 32 : cap * 2;
            struct dir_item *grown = realloc(items, cap * sizeof(*items));
            if (grown == NULL)
                break;
            items = grown;
        }

        items[count].name   = strdup(ent->d_name);
        items[count].is_dir = S_ISDIR(st.st_mode);
        items[count].size   = (long long)st.st_size;
        items[count].mtime  = st.st_mtime;
        if (items[count].name != NULL)
            count++;
    }

    closedir(dir);
    if (count > 1)
        qsort(items, count, sizeof(*items), compare_items);
    *items_out = items;
    return count;
}

/* sends an HTML page listing the files in a directory */
static void send_listing(int fd, const char *dir_path, const struct request *req,
                         int head_only, struct log_entry *entry)
{
    struct dir_item *items = NULL;
    int count = read_dir_items(dir_path, &items);
    if (count < 0) {
        send_error(fd, errno_status(errno), head_only, entry);
        return;
    }

    struct strbuf sb = { .data = malloc(4096), .cap = 4096 };
    if (sb.data == NULL)
        sb.failed = 1;

    char title[PATH_MAX * 6];
    if (html_escape(req->path, title, sizeof(title)) != 0)
        title[0] = '\0';

    sb_printf(&sb,
              "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\">"
              "<title>Index of %s</title>\n"
              "<style>body{font-family:sans-serif;margin:2rem}"
              "td{padding:.2rem 1.5rem .2rem 0}th{text-align:left}</style>\n"
              "</head><body>\n<h1>Index of %s</h1>\n<table>\n"
              "<tr><th>Name</th><th>Size</th><th>Modified</th></tr>\n",
              title, title);

    if (strcmp(req->path, "/") != 0)
        sb_printf(&sb, "<tr><td><a href=\"../\">../</a></td><td>-</td><td></td></tr>\n");

    for (int i = 0; i < count; i++) {
        char link[NAME_MAX * 3 + 1];
        char text[NAME_MAX * 6 + 1];
        char when[32];
        char size[32];
        struct tm tm;

        if (url_encode(items[i].name, link, sizeof(link)) != 0 ||
            html_escape(items[i].name, text, sizeof(text)) != 0)
            continue;

        localtime_r(&items[i].mtime, &tm);
        strftime(when, sizeof(when), "%d/%m/%Y %H:%M", &tm);

        if (items[i].is_dir)
            snprintf(size, sizeof(size), "-");
        else
            snprintf(size, sizeof(size), "%lld", items[i].size);

        sb_printf(&sb, "<tr><td><a href=\"%s%s\">%s%s</a></td><td>%s</td><td>%s</td></tr>\n",
                  link, items[i].is_dir ? "/" : "", text, items[i].is_dir ? "/" : "",
                  size, when);
    }

    sb_printf(&sb, "</table>\n<hr><p>%s</p>\n</body></html>\n", SERVER_NAME);

    for (int i = 0; i < count; i++)
        free(items[i].name);
    free(items);

    if (sb.failed) {
        free(sb.data);
        send_error(fd, 500, head_only, entry);
        return;
    }

    struct response_info info = {
        .content_type   = "text/html; charset=utf-8",
        .content_length = (long long)sb.len,
    };
    send_headers(fd, 200, &info);
    if (!head_only)
        write_all(fd, sb.data, sb.len);

    entry->status = 200;
    entry->bytes  = head_only ? 0 : (long long)sb.len;
    free(sb.data);
}

/* sends a 301 redirect that adds the missing trailing slash to a directory URL */
static void send_slash_redirect(int fd, const struct request *req, int head_only,
                                struct log_entry *entry)
{
    char   location[PATH_MAX + 2];
    size_t path_len = strcspn(req->target, "?#");

    if (path_len + 2 > sizeof(location)) {
        send_error(fd, 404, head_only, entry);
        return;
    }
    memcpy(location, req->target, path_len);
    location[path_len]     = '/';
    location[path_len + 1] = '\0';

    struct response_info info = { .content_length = 0, .location = location };
    send_headers(fd, 301, &info);
    entry->status = 301;
    entry->bytes  = 0;
}

/* answers a valid GET or HEAD request: file, index.html, listing, redirect or error */
void serve_path(int fd, const struct server_config *cfg, const struct request *req,
                struct log_entry *entry)
{
    int  head_only = strcmp(req->method, "HEAD") == 0;
    char full[PATH_MAX];

    int code = resolve_path(cfg->root, req->path, full, sizeof(full));
    if (code != 0) {
        send_error(fd, code, head_only, entry);
        return;
    }

    struct stat st;
    if (stat(full, &st) != 0) {
        send_error(fd, errno_status(errno), head_only, entry);
        return;
    }

    if (S_ISREG(st.st_mode)) {
        send_file(fd, full, req, head_only, entry);
        return;
    }

    if (!S_ISDIR(st.st_mode)) {
        send_error(fd, 403, head_only, entry);
        return;
    }

    size_t path_len = strlen(req->path);
    if (path_len == 0 || req->path[path_len - 1] != '/') {
        send_slash_redirect(fd, req, head_only, entry);
        return;
    }

    char        index[PATH_MAX];
    struct stat index_st;
    if (snprintf(index, sizeof(index), "%s/index.html", full) < (int)sizeof(index) &&
        stat(index, &index_st) == 0 && S_ISREG(index_st.st_mode)) {
        send_file(fd, index, req, head_only, entry);
        return;
    }

    send_listing(fd, full, req, head_only, entry);
}
