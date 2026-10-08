# SimpleHTTPd

A small HTTP file server written in C. It serves a folder over HTTP to browsers and `curl`, using only the C
library and POSIX system calls: sockets, `fork`, signals, `realpath`, `opendir` and `read`/`write`.

This project adds networking, signal handling, a text protocol and file-system security.

The server comes in two versions, plus a test suite:

- **Stage 1**: an iterative server. It serves files to one client at a time.
- **Stage 2**: the full server. It forks a process per connection, lists directories, supports `HEAD` and
  conditional requests, writes an access log, blocks path-traversal attacks, times out slow clients and shuts
  down cleanly.
- **Stage 3**: unit and integration tests run under AddressSanitizer and UBSan, plus a demo script.

## Project structure

```
Stage1/
  src/
    simplehttpd.c   socket setup and the accept loop
    http.c          request parsing, responses and file sending
    simplehttpd.h   shared constants and prototypes
  manual/
    readme.txt      user manual
  www/              sample site
  bin/              build output
  makefile
Stage2/
  src/
    simplehttpd.c   options, signals, sockets and the fork-per-connection loop
    http.c          reading and parsing requests, status lines and headers
    files.c         path resolution, MIME types, file sending and directory listings
    log.c           access log in Common Log Format
    simplehttpd.h   shared structures and prototypes
  manual/
    readme.txt
  www/
  bin/
  makefile
Stage3/
  tests/
    test_units.c    unit tests for the parser, path resolution and helpers
    integration.sh  starts both servers and checks them with curl
  demo.sh           scripted walkthrough, for recording a demo
  makefile          make test
```

## Requirements

- Linux (or WSL) with `gcc` and `make`
- `curl` for the tests and the demo

## Build

```bash
cd Stage2        # or Stage1
make
```

The binary is written to `bin/simplehttpd`. Both stages compile with `gcc -Wall -Wextra` and no warnings.

## Usage

### Stage 1

```bash
cd Stage1
./bin/simplehttpd [port] [root]      # defaults: 8080 and www
```

### Stage 2

```bash
cd Stage2
./bin/simplehttpd [-p port] [-d root] [-l logfile] [-t seconds] [-h]
```

| Option       | Description                                           | Default |
| ------------ | ----------------------------------------------------- | ------- |
| `-p port`    | Port to listen on                                     | `8080`  |
| `-d root`    | Folder to serve                                       | `www`   |
| `-l logfile` | Append the access log to this file                    | stdout  |
| `-t seconds` | Time a client has to send its request (and to read)   | `10`    |
| `-h`         | Show usage                                            |         |

Then open <http://localhost:8080/>. Stop the server with `Ctrl+C` or `kill <pid>`. It finishes the requests in
progress, then prints `simplehttpd: shutting down`.

### What gets served

| Request                         | Response                                                       |
| ------------------------------- | -------------------------------------------------------------- |
| A file                          | The file, with `Content-Type` chosen from its extension         |
| A folder, with a trailing `/`   | Its `index.html`, or else a generated listing of its files      |
| A folder, without the `/`       | `301` redirect to the same path with `/` added                  |
| `HEAD` instead of `GET`         | The same headers with no body                                   |
| `If-Modified-Since` is current  | `304 Not Modified` with no body                                 |

Listings show subfolders first, then files, with size and modification date. Hidden files (names starting with
`.`) are not listed. Names are HTML-escaped and their links are percent-encoded, so a file called `<b>.txt` or
`a b%.txt` displays and downloads correctly.

### Status codes (Stage 2)

| Code  | When                                                                   |
| ----- | ---------------------------------------------------------------------- |
| `200` | File or listing sent                                                   |
| `301` | Folder requested without a trailing slash                              |
| `304` | The client's cached copy is still current                              |
| `400` | Malformed request line or header, bad `%` escape, HTTP/1.1 without `Host` |
| `403` | The path leads outside the root, or the file cannot be read            |
| `404` | No such file or folder                                                 |
| `405` | Any method other than `GET` or `HEAD` (with `Allow: GET, HEAD`)         |
| `408` | The client did not send a full request within the timeout              |
| `431` | The request headers are larger than 8 KB                               |
| `500` | Unexpected server error                                                |
| `505` | HTTP version other than 1.0 or 1.1                                     |

### MIME types

`html htm css js json txt md xml png jpg jpeg gif svg ico webp pdf wasm`. Matching ignores case. Text types are
sent with `; charset=utf-8`, and anything else is `application/octet-stream`.

### Access log

One line per response, in [Common Log Format](https://en.wikipedia.org/wiki/Common_Log_Format) (client, time,
request line, status, body bytes):

```
127.0.0.1 - - [08/Oct/2026:10:58:55 +0100] "GET / HTTP/1.1" 200 416
127.0.0.1 - - [08/Oct/2026:10:58:55 +0100] "GET /docs/ HTTP/1.1" 200 588
127.0.0.1 - - [08/Oct/2026:10:58:55 +0100] "GET /../makefile HTTP/1.1" 403 133
```

### Example session

```
$ ./bin/simplehttpd -p 18080 -l access.log &
simplehttpd: serving '/home/user/simplehttpd/Stage2/www' on http://localhost:18080/ (pid 55747)

$ curl -i http://localhost:18080/
HTTP/1.1 200 OK
Date: Thu, 08 Oct 2026 09:58:55 GMT
Server: simplehttpd/1.0
Content-Type: text/html; charset=utf-8
Content-Length: 416
Last-Modified: Thu, 08 Oct 2026 09:58:43 GMT
Connection: close

<!DOCTYPE html>
...

$ curl -I http://localhost:18080/docs
HTTP/1.1 301 Moved Permanently
Date: Thu, 08 Oct 2026 09:58:55 GMT
Server: simplehttpd/1.0
Content-Length: 0
Location: /docs/
Connection: close

$ curl -s --path-as-is -o /dev/null -w '%{http_code}\n' http://localhost:18080/../makefile
403

$ curl -i -H 'If-Modified-Since: Thu, 08 Oct 2026 09:58:43 GMT' http://localhost:18080/style.css
HTTP/1.1 304 Not Modified
Date: Thu, 08 Oct 2026 09:58:55 GMT
Server: simplehttpd/1.0
Last-Modified: Thu, 08 Oct 2026 09:58:43 GMT
Connection: close

$ kill %1
simplehttpd: shutting down
```

## Tests

```bash
cd Stage3
make test
```

- **Unit tests** (`tests/test_units.c`, 121 checks): request-line and header parsing, the 32-header limit,
  percent-decoding, reading a request that arrives in pieces, path resolution against a temporary folder
  (including `..` and symlinks that escape the root, and a sibling folder `www2` beside `www`), MIME types,
  escaping, and the responses `serve_path` builds.
- **Integration tests** (`tests/integration.sh`, 56 checks): start both servers on port 18080 and test them with
  `curl` and raw sockets. They cover every status code, `HEAD`, redirects, listings, `index.html`, `304`, a 5 MB
  download compared byte for byte, a client that disconnects mid-download, a request split across two packets,
  a client that trickles one byte at a time, a redirect with a very long `Location`, a newline smuggled into the
  path, 20 parallel clients, no zombie processes, the log format, and a clean exit on `SIGTERM`.

Both run with `-fsanitize=address,undefined`, and any sanitizer report fails the run.

## Demo

`Stage3/demo.sh` starts the Stage 2 server and walks through its features one command at a time, pausing for
Enter between steps. It is meant for recording a demo video. `DEMO_NOPAUSE=1 ./demo.sh` runs straight through.

## How it works

1. **Listen:** `main` parses the options with `getopt`, resolves the root with `realpath`, and opens a TCP socket
   (`socket`, `setsockopt(SO_REUSEADDR)`, `bind`, `listen`).
2. **Accept and fork:** the parent loops on `accept`. For each connection it calls `fork`. The child closes the
   listening socket and handles the request. The parent closes its copy of the connection and goes back to
   `accept`.
3. **Reap:** a `SIGCHLD` handler calls `waitpid(-1, NULL, WNOHANG)` in a loop, so finished children never become
   zombies. It saves and restores `errno`, and is installed with `SA_RESTART` so `accept` isn't interrupted.
4. **Read:** the child calls `recv` until it sees the blank line that ends the headers (`\r\n\r\n`). Before
   each `recv` it sets `SO_RCVTIMEO` to the time left before a deadline, so the whole request must arrive within
   `-t` seconds. That holds even for a client that trickles one byte at a time (a "Slowloris" attack). The
   result is `408` on timeout, or `431` if 8 KB fills up first. `SO_SNDTIMEO` stops a client that never reads
   from holding a child forever.
5. **Parse:** `parse_request` splits the request line and up to 32 headers in place, and checks the method, the
   version and `Host`. It rejects control characters in the target, so nothing can be smuggled into the
   `Location` header. Then it removes the query string and percent-decodes the path.
6. **Resolve:** `resolve_path` joins the root and the path and calls `realpath`, which removes `..` and follows
   symlinks. The result must equal the root or start with the root followed by `/`. Anything else is `403`, so
   neither `/../etc/passwd`, `%2e%2e`, nor a symlink pointing outside can escape.
7. **Respond:** `serve_path` sends a file (64 KB `read`/`write` chunks, with `write_all` handling partial writes),
   `index.html`, a generated listing (`opendir`, `readdir`, `qsort`), a `301` or an error page.
8. **Log and close:** the child writes one log line with a single `write` to a file opened with `O_APPEND`, so
   lines from different children never mix. It then calls `shutdown(SHUT_WR)` and reads any input the client is
   still sending before `close`, so the connection isn't reset before the client reads the response. Then it
   exits.
9. **Shut down:** `SIGINT`/`SIGTERM` set a `volatile sig_atomic_t` flag. Their handlers are installed without
   `SA_RESTART`, so `accept` returns `EINTR` and the loop stops. The parent closes the socket, waits for the
   remaining children and exits with status 0. `SIGPIPE` is ignored, so a client that disconnects early only
   ends its own child's write.

## Limitations

- One request per connection (`Connection: close`). No keep-alive or pipelining.
- `GET` and `HEAD` only. No uploads, CGI, range requests, compression or HTTPS.
- IPv4 only.
- No limit on the number of concurrent children, so a flood of connections can create many processes.
- A `SIGTERM` that arrives in the instant between the loop's flag check and `accept` is only noticed after the
  next connection.
- Serving the whole file system (`-d /`) is refused with `403` for every path, because the containment check
  expects a root below `/`.
- Stage 1 serves one client at a time, does not decode `%` escapes, and only rejects paths containing `..`
  (no symlink checks).
