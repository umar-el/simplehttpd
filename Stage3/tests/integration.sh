#!/bin/bash
# integration.sh - starts the Stage 1 and Stage 2 servers and checks them with curl

set -u
cd "$(dirname "$0")/.." || exit 1

PORT=18080
HOST=127.0.0.1
BASE="http://$HOST:$PORT"
PASSED=0
FAILED=0

TMP=$(mktemp -d /tmp/shttpd-it.XXXXXX)
SITE="$TMP/site"
LOG="$TMP/access.log"
ERR="$TMP/stderr.log"
PID=""

# kills the server (if running) and removes the temporary files
cleanup()
{
    if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
        kill -KILL "$PID" 2>/dev/null
    fi
    case "$TMP" in
        /tmp/shttpd-it.*) rm -r -- "$TMP" ;;
    esac
}
trap cleanup EXIT

# check NAME EXPECTED ACTUAL - records one pass or failure
check()
{
    if [ "$2" == "$3" ]; then
        PASSED=$((PASSED + 1))
        echo "  pass  $1"
    else
        FAILED=$((FAILED + 1))
        echo "  FAIL  $1: expected '$2', got '$3'"
    fi
}

# status URL [curl options...] - prints the HTTP status code
status()
{
    local url=$1
    shift
    curl -s -o /dev/null -w '%{http_code}' "$@" "$url"
}

# header URL NAME [curl options...] - prints one response header's value
header()
{
    local url=$1 name=$2
    shift 2
    curl -s -D - -o /dev/null "$@" "$url" | tr -d '\r' | sed -n "s/^$name: //Ip" | head -1
}

# raw TEXT - sends TEXT over a plain TCP connection and prints the status line
raw()
{
    exec 3<>"/dev/tcp/$HOST/$PORT" || return
    printf '%b' "$1" >&3
    head -1 <&3 | tr -d '\r'
    exec 3<&-
}

# wait_for_port - waits up to 5 seconds for the server to accept connections
wait_for_port()
{
    for _ in $(seq 50); do
        if (exec 3<>"/dev/tcp/$HOST/$PORT") 2>/dev/null; then
            return 0
        fi
        sleep 0.1
    done
    echo "server did not start"
    exit 1
}

# builds the test site
make_site()
{
    mkdir -p "$SITE/dir/sub" "$SITE/withindex" "$TMP/outside"
    echo "hello world" > "$SITE/hello.txt"
    echo "<h1>home</h1>" > "$SITE/index.html"
    echo "notes" > "$SITE/dir/notes.txt"
    echo "bold" > "$SITE/dir/<b>.txt"
    echo "tricky name" > "$SITE/dir/a b%.txt"
    echo "<p>index page</p>" > "$SITE/withindex/index.html"
    echo "secret" > "$TMP/secret.txt"
    echo "outside" > "$TMP/outside/x.txt"
    ln -s "$TMP/secret.txt" "$SITE/escape"
    head -c 5000000 /dev/urandom > "$SITE/big.bin"
}

echo "Building"
make -s -C ../Stage1 || exit 1
mkdir -p bin
gcc -Wall -Wextra -g -fsanitize=address,undefined -fno-sanitize-recover=all \
    ../Stage2/src/*.c -o bin/simplehttpd-test || exit 1
make_site

echo "Stage 1"
../Stage1/bin/simplehttpd "$PORT" "$SITE" > "$LOG" 2> "$ERR" &
PID=$!
wait_for_port
check "s1 index"           200 "$(status "$BASE/")"
check "s1 body"            "<h1>home</h1>" "$(curl -s "$BASE/")"
check "s1 content type"    "text/plain; charset=utf-8" "$(header "$BASE/hello.txt" Content-Type)"
check "s1 not found"       404 "$(status "$BASE/nope")"
check "s1 dot-dot"         403 "$(status "$BASE/../secret.txt" --path-as-is)"
check "s1 post"            405 "$(status "$BASE/" -X POST)"
check "s1 garbage"         "HTTP/1.1 400 Bad Request" "$(raw 'garbage\r\n\r\n')"
check "s1 log line"        yes "$(grep -q '^127.0.0.1 "GET /hello.txt HTTP/1.1" 200$' "$LOG" && echo yes)"
kill "$PID"
wait "$PID" 2>/dev/null
PID=""
: > "$LOG"

echo "Stage 2"
ASAN_OPTIONS=detect_leaks=1 bin/simplehttpd-test -p "$PORT" -d "$SITE" -l "$LOG" -t 1 \
    > "$TMP/stdout.log" 2> "$ERR" &
PID=$!
wait_for_port

curl -s -o "$TMP/got" "$BASE/hello.txt"
check "file status"        200 "$(status "$BASE/hello.txt")"
check "file body"          same "$(cmp -s "$TMP/got" "$SITE/hello.txt" && echo same)"
check "index.html at /"    "<h1>home</h1>" "$(curl -s "$BASE/")"
check "query ignored"      200 "$(status "$BASE/hello.txt?v=2")"
check "head status"        200 "$(status "$BASE/hello.txt" -I)"
check "head length"        12 "$(header "$BASE/hello.txt" Content-Length -I)"
check "head no body"       0 "$(curl -s -I -o /dev/null -w '%{size_download}' "$BASE/hello.txt")"
check "not found"          404 "$(status "$BASE/nope.txt")"
check "dot-dot"            403 "$(status "$BASE/../secret.txt" --path-as-is)"
check "encoded dot-dot"    403 "$(status "$BASE/%2e%2e/secret.txt" --path-as-is)"
check "symlink outside"    403 "$(status "$BASE/escape")"
check "post"               405 "$(status "$BASE/" -X POST)"
check "allow header"       "GET, HEAD" "$(header "$BASE/" Allow -X POST)"
check "dir redirect"       301 "$(status "$BASE/dir")"
check "redirect location"  "/dir/" "$(header "$BASE/dir" Location)"
LISTING=$(curl -s "$BASE/dir/")
check "listing status"     200 "$(status "$BASE/dir/")"
check "listing file"       yes "$(grep -q 'href="notes.txt"' <<< "$LISTING" && echo yes)"
check "listing subdir"     yes "$(grep -q 'href="sub/"' <<< "$LISTING" && echo yes)"
check "listing escapes"    yes "$(grep -q '&lt;b&gt;.txt' <<< "$LISTING" && echo yes)"
check "listing no raw tag" no "$(grep -q '<b>.txt' <<< "$LISTING" && echo yes || echo no)"
check "dir index.html"     "<p>index page</p>" "$(curl -s "$BASE/withindex/")"

LINK=$(grep -o 'href="a[^"]*"' <<< "$LISTING" | sed 's/^href="//; s/"$//')
check "encoded link"       "a%20b%25.txt" "$LINK"
check "follow link"        "tricky name" "$(curl -s "$BASE/dir/$LINK")"

LAST_MOD=$(header "$BASE/hello.txt" Last-Modified)
check "not modified"       304 "$(status "$BASE/hello.txt" -H "If-Modified-Since: $LAST_MOD")"
check "modified since"     200 "$(status "$BASE/hello.txt" -H "If-Modified-Since: Thu, 01 Jan 1970 00:00:00 GMT")"

check "1.1 without host"   "HTTP/1.1 400 Bad Request" "$(raw 'GET / HTTP/1.1\r\n\r\n')"
check "1.0 without host"   "HTTP/1.1 200 OK" "$(raw 'GET / HTTP/1.0\r\n\r\n')"
check "bad version"        "HTTP/1.1 505 HTTP Version Not Supported" "$(raw 'GET / HTTP/2.0\r\n\r\n')"
check "bad escape"         "HTTP/1.1 400 Bad Request" "$(raw 'GET /%zz HTTP/1.0\r\n\r\n')"
BIG_HEADER=$(head -c 9000 /dev/zero | tr '\0' a)
check "huge header"        "HTTP/1.1 431 Request Header Fields Too Large" \
                           "$(raw "GET / HTTP/1.0\r\nX-Big: $BIG_HEADER\r\n\r\n")"

exec 4<>"/dev/tcp/$HOST/$PORT"
START=$SECONDS
TIMEOUT_LINE=$(timeout 5 head -1 <&4 | tr -d '\r')
exec 4<&-
check "idle client"        "HTTP/1.1 408 Request Timeout" "$TIMEOUT_LINE"
check "idle within 3s"     yes "$([ $((SECONDS - START)) -le 3 ] && echo yes)"

exec 6<>"/dev/tcp/$HOST/$PORT"
START=$SECONDS
( for c in G E T ' ' / ' ' H T T P; do printf '%s' "$c"; sleep 0.5; done ) >&6 2>/dev/null &
TRICKLE=$!
TRICKLE_LINE=$(timeout 6 head -1 <&6 | tr -d '\r')
kill "$TRICKLE" 2>/dev/null
wait "$TRICKLE" 2>/dev/null
exec 6<&-
check "trickling client"   "HTTP/1.1 408 Request Timeout" "$TRICKLE_LINE"
check "trickle within 3s"  yes "$([ $((SECONDS - START)) -le 3 ] && echo yes)"

LONG_DIR="/dir$(printf '/.%.0s' $(seq 700))"
check "long redirect"      301 "$(status "$BASE$LONG_DIR" --path-as-is)"
check "long location"      "$LONG_DIR/" "$(header "$BASE$LONG_DIR" Location --path-as-is)"
check "newline in target"  "HTTP/1.1 400 Bad Request" "$(raw 'GET /dir\nX-Injected:1 HTTP/1.0\r\n\r\n')"

exec 5<>"/dev/tcp/$HOST/$PORT"
printf 'GET /hello.txt HT' >&5
sleep 0.3
printf 'TP/1.0\r\n\r\n' >&5
check "split request"      "HTTP/1.1 200 OK" "$(head -1 <&5 | tr -d '\r')"
exec 5<&-

curl -s -o "$TMP/big" "$BASE/big.bin"
check "large file"         same "$(cmp -s "$TMP/big" "$SITE/big.bin" && echo same)"
curl -s "$BASE/big.bin" | head -c 1000 > /dev/null
check "after abort"        200 "$(status "$BASE/hello.txt")"

CLIENTS=()
for i in $(seq 20); do
    status "$BASE/hello.txt" > "$TMP/par.$i" &
    CLIENTS+=($!)
done
wait "${CLIENTS[@]}"
check "parallel requests"  20 "$(grep -lx 200 "$TMP"/par.* | wc -l)"

sleep 1
check "no zombies"         0 "$(ps --ppid "$PID" -o stat= | grep -c Z)"
check "log format"         yes "$(grep -Eq '^127\.0\.0\.1 - - \[[^]]+\] "GET / HTTP/1\.1" 200 [0-9]+$' "$LOG" && echo yes)"
check "log 404 line"       yes "$(grep -Eq '"GET /nope.txt HTTP/1\.1" 404 [0-9]+$' "$LOG" && echo yes)"
check "log line count"     "$(wc -l < "$LOG")" "$(grep -Ec '^127\.0\.0\.1 - - \[[^]]+\] ".*" [0-9]{3} [0-9]+$' "$LOG")"

kill -TERM "$PID"
wait "$PID"
check "sigterm exit"       0 "$?"
PID=""
check "shutdown message"   yes "$(grep -q 'shutting down' "$TMP/stdout.log" && echo yes)"
check "sanitizer clean"    "" "$(grep -E 'runtime error|AddressSanitizer|LeakSanitizer' "$ERR")"

echo
echo "$PASSED passed, $FAILED failed"
[ "$FAILED" -eq 0 ]
