#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PORT="${PORT:-18080}"
SERVER="$ROOT_DIR/server"
LOG="${TMPDIR:-/tmp}/threaded-db-api.log"

fail() {
    echo "api_test failed: $1" >&2
    if [ -f "$LOG" ]; then
        cat "$LOG" >&2
    fi
    exit 1
}

"$SERVER" --port "$PORT" --workers 1 >"$LOG" 2>&1 &
PID=$!
trap 'kill "$PID" 2>/dev/null || true; wait "$PID" 2>/dev/null || true' EXIT

i=0
while [ "$i" -lt 50 ]; do
    if curl -fsS "http://127.0.0.1:$PORT/health" >/tmp/threaded-db-health.out 2>/dev/null; then
        break
    fi
    i=$((i + 1))
    sleep 0.1
done

grep -q '"ok":true' /tmp/threaded-db-health.out || fail "health failed"

curl -fsS -X POST "http://127.0.0.1:$PORT/sql" \
    -H 'Content-Type: application/json' \
    -d '{"sql":"CREATE TABLE books;"}' | grep -q '"ok":true' || fail "create failed"

curl -fsS -X POST "http://127.0.0.1:$PORT/sql" \
    -H 'Content-Type: application/json' \
    -d '{"sql":"INSERT INTO books VALUES (1, '\''C Book'\'', '\''Kim'\'', 2024);"}' |
    grep -q '"affected":1' || fail "insert failed"

curl -fsS -X POST "http://127.0.0.1:$PORT/sql" \
    -H 'Content-Type: application/json' \
    -d '{"sql":"SELECT * FROM books WHERE id = 1;"}' |
    grep -q '"title":"C Book"' || fail "select failed"

curl -fsS -X POST "http://127.0.0.1:$PORT/sql" \
    -H 'Content-Type: application/json' \
    -d '{"sql":"DELETE FROM books WHERE id = 1;"}' |
    grep -q '"affected":1' || fail "delete failed"

curl -fsS "http://127.0.0.1:$PORT/stats" | grep -q '"workers":1' || fail "stats failed"

code=$(curl -sS -o /tmp/threaded-db-bad-json.out -w '%{http_code}' \
    -X POST "http://127.0.0.1:$PORT/sql" \
    -H 'Content-Type: application/json' \
    -d '{"bad":"json"}')
[ "$code" = "400" ] || fail "bad json should return 400"

curl -sS -X POST "http://127.0.0.1:$PORT/sql" \
    -H 'Content-Type: application/json' \
    -d '{"sql":"BAD SQL;"}' |
    grep -q '"ok":false' || fail "bad sql failed"

i=1
pids=""
while [ "$i" -le 120 ]; do
    curl --max-time 5 -fsS -X POST "http://127.0.0.1:$PORT/sql" \
        -H 'Content-Type: application/json' \
        -d "{\"sql\":\"SELECT * FROM books WHERE id = $i;\"}" >/dev/null &
    pids="$pids $!"
    i=$((i + 1))
done
for pid in $pids; do
    wait "$pid"
done

curl -fsS -X POST "http://127.0.0.1:$PORT/bench" \
    -H 'Content-Type: application/json' \
    -d '{"mode":"read","count":10}' |
    grep -q '"mode":"read"' || fail "bench endpoint failed"

echo "api_test: OK"
