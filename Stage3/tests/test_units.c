/* test_units.c - unit tests for the Stage 2 parser and helpers */

#include "simplehttpd.h"

static int checks   = 0;
static int failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        checks++;                                                          \
        if (!(cond)) {                                                     \
            failures++;                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                  \
    } while (0)

#define CHECK_STR(actual, expected) \
    CHECK((actual) != NULL && strcmp((actual), (expected)) == 0)

/* parses a copy of text, so string literals can be passed in */
static int parse(const char *text, struct request *req)
{
    static char buf[REQUEST_BUFSIZE];
    snprintf(buf, sizeof(buf), "%s", text);
    return parse_request(buf, req);
}

/* checks request line, header, and path decoding */
static void test_parse_request(void)
{
    struct request req;

    CHECK(parse("GET /a%20b?x=1 HTTP/1.1\r\nHost: h\r\nX-Y: z\r\n\r\n", &req) == 0);
    CHECK_STR(req.method, "GET");
    CHECK_STR(req.target, "/a%20b?x=1");
    CHECK_STR(req.version, "HTTP/1.1");
    CHECK_STR(req.path, "/a b");
    CHECK_STR(get_header(&req, "x-y"), "z");
    CHECK_STR(get_header(&req, "HOST"), "h");
    CHECK(get_header(&req, "Missing") == NULL);

    CHECK(parse("HEAD /x HTTP/1.0\r\nAccept:   */*  \r\n\r\n", &req) == 0);
    CHECK_STR(req.method, "HEAD");
    CHECK_STR(get_header(&req, "accept"), "*/*");

    CHECK(parse("GET / HTTP/1.1\r\n\r\n", &req) == 400);
    CHECK(parse("GET / HTTP/1.0\r\n\r\n", &req) == 0);
    CHECK(parse("GET / HTTP/2.0\r\n\r\n", &req) == 505);
    CHECK(parse("GET / FTP/1.0\r\n\r\n", &req) == 400);
    CHECK(parse("GET / HTTP/a.b\r\n\r\n", &req) == 400);
    CHECK(parse("POST / HTTP/1.0\r\n\r\n", &req) == 405);
    CHECK(parse("GET\r\n\r\n", &req) == 400);
    CHECK(parse("GET / HTTP/1.0 extra\r\n\r\n", &req) == 400);
    CHECK(parse("GET nopath HTTP/1.0\r\n\r\n", &req) == 400);
    CHECK(parse("GET /%zz HTTP/1.0\r\n\r\n", &req) == 400);
    CHECK(parse("GET / HTTP/1.0\r\nno colon here\r\n\r\n", &req) == 400);
    CHECK(parse("GET / HTTP/1.0\r\n: empty name\r\n\r\n", &req) == 400);
    CHECK(parse("GET /a\nb HTTP/1.0\r\n\r\n", &req) == 400);
    CHECK(parse("GET /a\x01" "b HTTP/1.0\r\n\r\n", &req) == 400);
    CHECK(parse("GET /a\x7f" "b HTTP/1.0\r\n\r\n", &req) == 400);
}

/* builds a request with n headers and checks the MAX_HEADERS limit */
static void test_header_limit(void)
{
    struct request req;
    char text[REQUEST_BUFSIZE];

    for (int n = MAX_HEADERS; n <= MAX_HEADERS + 1; n++) {
        int len = snprintf(text, sizeof(text), "GET / HTTP/1.0\r\n");
        for (int i = 0; i < n; i++)
            len += snprintf(text + len, sizeof(text) - len, "H%d: v\r\n", i);
        snprintf(text + len, sizeof(text) - len, "\r\n");

        CHECK(parse(text, &req) == (n == MAX_HEADERS ? 0 : 400));
    }
}

/* checks percent-decoding and its error cases */
static void test_url_decode(void)
{
    char out[64];

    CHECK(url_decode("%41%2f+", out, sizeof(out)) == 0);
    CHECK_STR(out, "A/+");
    CHECK(url_decode("/plain", out, sizeof(out)) == 0);
    CHECK_STR(out, "/plain");
    CHECK(url_decode("%C3%A9", out, sizeof(out)) == 0);
    CHECK_STR(out, "\xc3\xa9");
    CHECK(url_decode("%4", out, sizeof(out)) == -1);
    CHECK(url_decode("%zz", out, sizeof(out)) == -1);
    CHECK(url_decode("%00", out, sizeof(out)) == -1);
    CHECK(url_decode("abcdef", out, 4) == -1);
}

/* checks reason phrases */
static void test_status_text(void)
{
    CHECK_STR(status_text(200), "OK");
    CHECK_STR(status_text(404), "Not Found");
    CHECK_STR(status_text(431), "Request Header Fields Too Large");
    CHECK_STR(status_text(505), "HTTP Version Not Supported");
}

/* checks read_request over a socketpair: split writes, overflow, early close */
static void test_read_request(void)
{
    int sv[2];
    char buf[64];
    size_t len;

    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    CHECK(write(sv[1], "GET / HT", 8) == 8);
    CHECK(write(sv[1], "TP/1.0\r\n\r\n", 10) == 10);
    CHECK(read_request(sv[0], buf, sizeof(buf), &len, 5) == 0);
    CHECK(len == 18);
    CHECK_STR(buf, "GET / HTTP/1.0\r\n\r\n");
    close(sv[0]);
    close(sv[1]);

    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    char big[100];
    memset(big, 'a', sizeof(big));
    CHECK(write(sv[1], big, sizeof(big)) == (ssize_t)sizeof(big));
    CHECK(read_request(sv[0], buf, sizeof(buf), &len, 5) == 431);
    close(sv[0]);
    close(sv[1]);

    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    close(sv[1]);
    CHECK(read_request(sv[0], buf, sizeof(buf), &len, 5) == -1);
    close(sv[0]);

    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    CHECK(write(sv[1], "GET / HTTP/1.0\r\n", 16) == 16);
    close(sv[1]);
    CHECK(read_request(sv[0], buf, sizeof(buf), &len, 5) == 400);
    close(sv[0]);
}

static char tmp_dir[256];
static char root[PATH_MAX];

/* writes text to dir/name */
static void make_file(const char *name, const char *text)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", tmp_dir, name);
    FILE *f = fopen(path, "w");
    if (f != NULL) {
        fputs(text, f);
        fclose(f);
    }
}

/* builds the temporary site used by the file-system tests */
static void make_site(void)
{
    char path[PATH_MAX];

    snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/shttpd.XXXXXX");
    CHECK(mkdtemp(tmp_dir) != NULL);

    snprintf(path, sizeof(path), "%s/www", tmp_dir);
    CHECK(mkdir(path, 0755) == 0);
    snprintf(path, sizeof(path), "%s/www/sub", tmp_dir);
    CHECK(mkdir(path, 0755) == 0);
    snprintf(path, sizeof(path), "%s/www2", tmp_dir);
    CHECK(mkdir(path, 0755) == 0);

    make_file("www/a.txt", "hello\n");
    make_file("www/.hidden", "secret\n");
    make_file("secret.txt", "top secret\n");
    make_file("www2/x.txt", "sibling\n");

    snprintf(path, sizeof(path), "%s/www/out", tmp_dir);
    CHECK(symlink("../secret.txt", path) == 0);
    snprintf(path, sizeof(path), "%s/www/sib", tmp_dir);
    CHECK(symlink("../www2/x.txt", path) == 0);
    snprintf(path, sizeof(path), "%s/www/inside", tmp_dir);
    CHECK(symlink("a.txt", path) == 0);

    snprintf(path, sizeof(path), "%s/www", tmp_dir);
    CHECK(realpath(path, root) != NULL);
}

/* removes the temporary site */
static void remove_site(void)
{
    char cmd[PATH_MAX + 16];
    snprintf(cmd, sizeof(cmd), "rm -r '%s'", tmp_dir);
    CHECK(system(cmd) == 0);
}

/* checks that paths are confined to the document root */
static void test_resolve_path(void)
{
    char out[PATH_MAX];
    char expected[PATH_MAX + 8];

    CHECK(resolve_path(root, "/a.txt", out, sizeof(out)) == 0);
    snprintf(expected, sizeof(expected), "%s/a.txt", root);
    CHECK_STR(out, expected);

    CHECK(resolve_path(root, "/", out, sizeof(out)) == 0);
    CHECK_STR(out, root);

    CHECK(resolve_path(root, "/sub/", out, sizeof(out)) == 0);
    CHECK(resolve_path(root, "/inside", out, sizeof(out)) == 0);
    CHECK(resolve_path(root, "/../secret.txt", out, sizeof(out)) == 403);
    CHECK(resolve_path(root, "/sub/../../secret.txt", out, sizeof(out)) == 403);
    CHECK(resolve_path(root, "/out", out, sizeof(out)) == 403);
    CHECK(resolve_path(root, "/sib", out, sizeof(out)) == 403);
    CHECK(resolve_path(root, "/missing", out, sizeof(out)) == 404);
    CHECK(resolve_path(root, "/a.txt/x", out, sizeof(out)) == 404);
}

/* checks the extension table */
static void test_mime_type(void)
{
    CHECK_STR(mime_type("x.HTML"), "text/html; charset=utf-8");
    CHECK_STR(mime_type("a.png"), "image/png");
    CHECK_STR(mime_type("/dir.d/style.css"), "text/css; charset=utf-8");
    CHECK_STR(mime_type("noext"), "application/octet-stream");
    CHECK_STR(mime_type("/dir.d/noext"), "application/octet-stream");
    CHECK_STR(mime_type("a.weird"), "application/octet-stream");
}

/* checks HTML escaping and URL encoding */
static void test_escaping(void)
{
    char out[128];
    char back[128];

    CHECK(html_escape("<a href=\"x\">&'", out, sizeof(out)) == 0);
    CHECK_STR(out, "&lt;a href=&quot;x&quot;&gt;&amp;&#39;");
    CHECK(html_escape("<<<<", out, 8) == -1);

    CHECK(url_encode("a b%/\xc3\xa9", out, sizeof(out)) == 0);
    CHECK_STR(out, "a%20b%25/%C3%A9");
    CHECK(url_encode("a b%.txt", out, sizeof(out)) == 0);
    CHECK(url_decode(out, back, sizeof(back)) == 0);
    CHECK_STR(back, "a b%.txt");
    CHECK(url_encode("    ", out, 8) == -1);
}

/* runs serve_path for a raw request and returns the response text */
static char *serve(const char *raw)
{
    static char response[65536];
    struct request req;
    struct log_entry entry;
    struct server_config cfg;
    int sv[2];

    memset(&entry, 0, sizeof(entry));
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.root, sizeof(cfg.root), "%s", root);

    response[0] = '\0';
    if (parse(raw, &req) != 0 || socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
        return response;

    serve_path(sv[1], &cfg, &req, &entry);
    close(sv[1]);

    size_t len = 0;
    ssize_t n;
    while (len < sizeof(response) - 1 &&
           (n = read(sv[0], response + len, sizeof(response) - 1 - len)) > 0)
        len += n;
    response[len] = '\0';
    close(sv[0]);
    return response;
}

/* checks the responses serve_path builds */
static void test_serve_path(void)
{
    char *r;

    r = serve("GET /a.txt HTTP/1.0\r\n\r\n");
    CHECK(strncmp(r, "HTTP/1.1 200 OK\r\n", 17) == 0);
    CHECK(strstr(r, "Content-Type: text/plain; charset=utf-8\r\n") != NULL);
    CHECK(strstr(r, "Content-Length: 6\r\n") != NULL);
    CHECK(strstr(r, "Last-Modified: ") != NULL);
    CHECK(strstr(r, "\r\n\r\nhello\n") != NULL);

    r = serve("HEAD /a.txt HTTP/1.0\r\n\r\n");
    CHECK(strstr(r, "Content-Length: 6\r\n") != NULL);
    CHECK(strstr(r, "hello") == NULL);

    r = serve("GET /a.txt HTTP/1.0\r\nIf-Modified-Since: Fri, 01 Jan 2100 00:00:00 GMT\r\n\r\n");
    CHECK(strncmp(r, "HTTP/1.1 304 Not Modified\r\n", 27) == 0);
    CHECK(strstr(r, "hello") == NULL);

    r = serve("GET /a.txt HTTP/1.0\r\nIf-Modified-Since: Thu, 01 Jan 1970 00:00:00 GMT\r\n\r\n");
    CHECK(strncmp(r, "HTTP/1.1 200 OK\r\n", 17) == 0);

    r = serve("GET /a.txt HTTP/1.0\r\nIf-Modified-Since: garbage\r\n\r\n");
    CHECK(strncmp(r, "HTTP/1.1 200 OK\r\n", 17) == 0);

    r = serve("GET /sub?x=1 HTTP/1.0\r\n\r\n");
    CHECK(strncmp(r, "HTTP/1.1 301 Moved Permanently\r\n", 32) == 0);
    CHECK(strstr(r, "Location: /sub/\r\n") != NULL);

    r = serve("GET / HTTP/1.0\r\n\r\n");
    CHECK(strncmp(r, "HTTP/1.1 200 OK\r\n", 17) == 0);
    CHECK(strstr(r, "Content-Type: text/html; charset=utf-8\r\n") != NULL);
    CHECK(strstr(r, "href=\"a.txt\"") != NULL);
    CHECK(strstr(r, ".hidden") == NULL);
    char *sub = strstr(r, "href=\"sub/\"");
    char *a   = strstr(r, "href=\"a.txt\"");
    CHECK(sub != NULL && a != NULL && sub < a);
    CHECK(strstr(r, "href=\"../\"") == NULL);

    r = serve("GET /sub/ HTTP/1.0\r\n\r\n");
    CHECK(strstr(r, "href=\"../\"") != NULL);

    r = serve("GET /out HTTP/1.0\r\n\r\n");
    CHECK(strncmp(r, "HTTP/1.1 403 Forbidden\r\n", 24) == 0);

    r = serve("GET /missing HTTP/1.0\r\n\r\n");
    CHECK(strncmp(r, "HTTP/1.1 404 Not Found\r\n", 24) == 0);

    make_file("www/sub/index.html", "<p>index</p>\n");
    r = serve("GET /sub/ HTTP/1.0\r\n\r\n");
    CHECK(strstr(r, "\r\n\r\n<p>index</p>\n") != NULL);
}

/* checks that send_headers refuses headers that do not fit instead of overflowing */
static void test_send_headers_limits(void)
{
    int sv[2];
    char location[6000];
    char reply[8192];

    memset(location, 'a', sizeof(location) - 1);
    location[0] = '/';
    location[sizeof(location) - 1] = '\0';

    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    struct response_info big = { .content_length = 0, .location = location };
    CHECK(send_headers(sv[1], 301, &big) == -1);

    location[PATH_MAX] = '\0';
    struct response_info fits = { .content_length = 0, .location = location };
    CHECK(send_headers(sv[1], 301, &fits) > PATH_MAX);
    close(sv[1]);

    ssize_t n = read(sv[0], reply, sizeof(reply) - 1);
    CHECK(n > PATH_MAX);
    if (n > 0) {
        reply[n] = '\0';
        CHECK(strncmp(reply, "HTTP/1.1 301 ", 13) == 0);
        CHECK(strstr(reply, "\r\nConnection: close\r\n\r\n") != NULL);
    }
    close(sv[0]);
}

int main(void)
{
    test_parse_request();
    test_header_limit();
    test_url_decode();
    test_status_text();
    test_read_request();
    test_send_headers_limits();

    make_site();
    test_resolve_path();
    test_mime_type();
    test_escaping();
    test_serve_path();
    remove_site();

    if (failures > 0) {
        fprintf(stderr, "%d of %d checks failed\n", failures, checks);
        return EXIT_FAILURE;
    }

    printf("all %d checks passed\n", checks);
    return EXIT_SUCCESS;
}
