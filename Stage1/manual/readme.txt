SimpleHTTPd User Manual - Stage 1

USAGE
-----
  ./bin/simplehttpd [port] [root]

  port   TCP port to listen on (default 8080)
  root   folder to serve files from (default www)

Open http://localhost:8080/ in a browser, or use curl:

  curl -i http://localhost:8080/

Stop the server with Ctrl+C.

WHAT IT DOES
------------
The Stage 1 server handles one client at a time. For each
connection it reads the request, sends back one response and
closes the connection.

  GET /               serves root/index.html
  GET /path/file.css  serves root/path/file.css

The Content-Type header is chosen from the file extension
(html, htm, css, js, txt, png, jpg, jpeg, gif, ico). Any other
file is sent as application/octet-stream.

STATUS CODES
------------
  200 OK                     the file was sent
  400 Bad Request            the request line could not be parsed
  403 Forbidden              the path contains "..", or the file
                             cannot be read
  404 Not Found              there is no such file
  405 Method Not Allowed     only GET is supported
  500 Internal Server Error  any other error

LOGGING
-------
One line is printed for every request:

  127.0.0.1 "GET /index.html HTTP/1.1" 200

LIMITATIONS
-----------
Only one client is served at a time, so a slow client blocks
everyone else. Directories cannot be listed. See Stage 2 for
the full server.
