#!/bin/bash
# demo.sh - walks through the Stage 2 server's features, one step at a time
# Set DEMO_NOPAUSE=1 to run without waiting for Enter.

set -u
cd "$(dirname "$0")/../Stage2" || exit 1

PORT=18080
BASE="http://localhost:$PORT"

# step TITLE COMMAND... - prints the title and command, runs it, then pauses
step()
{
    local title=$1
    shift
    echo
    echo "== $title"
    echo "\$ $*"
    "$@"
    if [ "${DEMO_NOPAUSE:-0}" != 1 ]; then
        read -r -p "(press Enter) " _
    fi
}

make -s || exit 1
LOG=$(mktemp /tmp/simplehttpd-demo.XXXXXX)
./bin/simplehttpd -p "$PORT" -l "$LOG" &
PID=$!
trap 'kill -TERM $PID 2>/dev/null; wait $PID; rm -f -- "$LOG"' EXIT
sleep 0.5

step "A file, with its headers"         curl -si "$BASE/"
step "HEAD: headers only"               curl -sI "$BASE/style.css"
step "A folder without a slash"         curl -sI "$BASE/docs"
step "A generated directory listing"    curl -s "$BASE/docs/"
step "Not found"                        curl -s -o /dev/null -w '%{http_code}\n' "$BASE/missing.html"
step "Path traversal is refused"        curl -s --path-as-is -o /dev/null -w '%{http_code}\n' "$BASE/../makefile"
step "Encoded traversal is refused too" curl -s --path-as-is -o /dev/null -w '%{http_code}\n' "$BASE/%2e%2e/makefile"
step "Only GET and HEAD are allowed"    curl -si -X POST "$BASE/"
LAST=$(curl -sI "$BASE/style.css" | tr -d '\r' | sed -n 's/^Last-Modified: //p')
step "Conditional request"              curl -si -H "If-Modified-Since: $LAST" "$BASE/style.css"
step "Ten clients at once"              bash -c "for i in \$(seq 10); do curl -s -o /dev/null -w '%{http_code} ' $BASE/ & done; wait; echo"
step "Child processes left (zombies)"   bash -c "ps --ppid $PID -o pid=,stat= | grep -c Z || true"
step "The access log"                  tail -n 5 "$LOG"
