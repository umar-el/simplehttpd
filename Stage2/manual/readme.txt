SimpleHTTPd User Manual - Stage 2

USAGE
-----
  ./bin/simplehttpd [-p port] [-d root] [-l logfile] [-t seconds] [-h]

  -p port      TCP port to listen on (default 8080)
  -d root      folder to serve (default www)
  -l logfile   append the access log to this file (default stdout)
  -t seconds   time a client has to send its request (default 10)
  -h           show usage

Open http://localhost:8080/ in a browser, or use curl:

  curl -i http://localhost:8080/

Stop the server with Ctrl+C, or with kill <pid>. Requests that
are in progress are finished first.

WHAT IT SERVES
--------------
  /file.txt     the file; Content-Type comes from the extension
  /folder/      folder/index.html if there is one, otherwise a
                listing of the folder (hidden files are left out)
  /folder       redirect (301) to /folder/

HEAD requests get the same headers as GET, with no body.
A request with If-Modified-Since gets 304 Not Modified when the
file has not changed since that date.

STATUS CODES
------------
  200 OK                               file or listing sent
  301 Moved Permanently                folder without trailing /
  304 Not Modified                     cached copy still current
  400 Bad Request                      malformed request, bad %
                                       escape, or HTTP/1.1
                                       without a Host header
  403 Forbidden                        path outside the root, or
                                       the file cannot be read
  404 Not Found                        no such file or folder
  405 Method Not Allowed               only GET and HEAD work
  408 Request Timeout                  request not sent in time
  431 Request Header Fields Too Large  headers over 8 KB
  500 Internal Server Error            unexpected error
  505 HTTP Version Not Supported       not HTTP/1.0 or HTTP/1.1

SECURITY
--------
Every path is resolved with realpath(), which removes ".." and
follows symbolic links. If the result is not inside the root
folder the server answers 403, so /../secret, %2e%2e/secret and
symlinks that point outside the root are all refused.

ACCESS LOG
----------
One line per response, in Common Log Format:

  127.0.0.1 - - [08/Oct/2026:10:58:55 +0100] "GET / HTTP/1.1" 200 416

The fields are: client address, time, request line, status code
and the number of body bytes sent.

PROCESSES AND SIGNALS
---------------------
Each connection is handled by a child process created with
fork(). The server reaps finished children when SIGCHLD arrives,
so no zombie processes are left behind. SIGINT and SIGTERM stop
the server cleanly. SIGPIPE is ignored, so a client that hangs
up early does not crash anything.
