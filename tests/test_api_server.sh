#!/bin/sh

set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
TMP_DB=$(mktemp -d /tmp/mini_db_server_test.XXXXXX)
LOG_FILE=$(mktemp /tmp/mini_db_server_test_log.XXXXXX)
SERVER_PID=""

pick_free_port() {
    python3 - <<'PY'
import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
PY
}

cleanup() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" >/dev/null 2>&1 || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    rm -rf "$TMP_DB" "$LOG_FILE"
}

start_server() {
    PORT="$1"
    THREADS="$2"
    QUEUE_SIZE="$3"

    "$ROOT_DIR/mini_db_server" -d "$TMP_DB" -p "$PORT" -t "$THREADS" -q "$QUEUE_SIZE" >"$LOG_FILE" 2>&1 &
    SERVER_PID=$!
}

stop_server() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" >/dev/null 2>&1 || true
        wait "$SERVER_PID" 2>/dev/null || true
        SERVER_PID=""
    fi
}

trap cleanup EXIT INT TERM

cp "$ROOT_DIR"/db/*.schema "$TMP_DB"/
: >"$TMP_DB/users.data"
: >"$TMP_DB/products.data"

PORT=$(pick_free_port)
start_server "$PORT" 4 64

PORT="$PORT" python3 - <<'PY'
import concurrent.futures
import http.client
import json
import os
import time

port = int(os.environ["PORT"])


def request(method, path, body=None, headers=None, timeout=5):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=timeout)
    conn.request(method, path, body=body, headers=headers or {})
    resp = conn.getresponse()
    text = resp.read().decode("utf-8")
    status = resp.status
    conn.close()
    return status, json.loads(text)


def query(sql):
    return request(
        "POST",
        "/query",
        body=json.dumps({"sql": sql}),
        headers={"Content-Type": "application/json"},
    )


deadline = time.time() + 5
while True:
    try:
        status, payload = request("GET", "/health")
        if status == 200 and payload == {"success": True, "service": "mini_db_server"}:
            break
    except Exception:
        pass

    if time.time() >= deadline:
        raise SystemExit("server did not become healthy")
    time.sleep(0.1)

status, payload = request("GET", "/health")
assert status == 200, payload
assert payload["success"] is True
assert payload["service"] == "mini_db_server"

status, payload = query("INSERT INTO users (name, age) VALUES ('api_user', '25');")
assert status == 200, payload
assert payload["type"] == "insert"
assert payload["affected_rows"] == 1
assert payload["generated_id"] == 1

status, payload = query("SELECT * FROM users WHERE id = 1;")
assert status == 200, payload
assert payload["type"] == "select"
assert payload["used_index"] is True
assert payload["row_count"] == 1
assert payload["rows"] == [["1", "api_user", "25"]]

status, payload = request(
    "POST",
    "/query",
    body="{",
    headers={"Content-Type": "application/json"},
)
assert status == 400, payload
assert payload["error_code"] == "INVALID_JSON"

status, payload = query("SELECT * FROM users; INSERT INTO users (name, age) VALUES ('bad', '1');")
assert status == 400, payload
assert payload["error_code"] == "MULTI_STATEMENT_NOT_ALLOWED"

with concurrent.futures.ThreadPoolExecutor(max_workers=8) as executor:
    select_responses = list(executor.map(lambda _: query("SELECT * FROM users WHERE id = 1;"), range(16)))

for status, payload in select_responses:
    assert status == 200, payload
    assert payload["used_index"] is True
    assert payload["row_count"] == 1


def insert_task(index):
    return query(f"INSERT INTO users (name, age) VALUES ('concurrent_{index}', '{20 + index}');")


with concurrent.futures.ThreadPoolExecutor(max_workers=8) as executor:
    insert_responses = list(executor.map(insert_task, range(10)))

generated_ids = []
for status, payload in insert_responses:
    assert status == 200, payload
    assert payload["type"] == "insert"
    generated_ids.append(payload["generated_id"])

assert len(set(generated_ids)) == 10

status, payload = query("SELECT * FROM users;")
assert status == 200, payload
assert payload["row_count"] == 11, payload
names = {row[1] for row in payload["rows"]}
assert "api_user" in names
for index in range(10):
    assert f"concurrent_{index}" in names
PY

stop_server

PORT=$(pick_free_port)
start_server "$PORT" 1 1

PORT="$PORT" python3 - <<'PY'
import http.client
import json
import os
import socket
import time

port = int(os.environ["PORT"])


def request(method, path, body=None, headers=None, timeout=5):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=timeout)
    conn.request(method, path, body=body, headers=headers or {})
    resp = conn.getresponse()
    text = resp.read().decode("utf-8")
    status = resp.status
    conn.close()
    return status, json.loads(text)


deadline = time.time() + 5
while True:
    try:
        status, payload = request("GET", "/health")
        if status == 200 and payload["success"] is True:
            break
    except Exception:
        pass

    if time.time() >= deadline:
        raise SystemExit("queue-full test server did not become healthy")
    time.sleep(0.1)


def open_blocker():
    sock = socket.create_connection(("127.0.0.1", port), timeout=5)
    sock.sendall(
        (
            f"POST /query HTTP/1.1\r\n"
            f"Host: 127.0.0.1:{port}\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: 100\r\n"
            "\r\n"
            '{"sql":"SELECT * FROM users WHERE id = 1;'
        ).encode("utf-8")
    )
    return sock


blockers = [open_blocker(), open_blocker()]
time.sleep(0.3)

try:
    deadline = time.time() + 3
    while True:
        try:
            status, payload = request(
                "POST",
                "/query",
                body=json.dumps({"sql": "SELECT * FROM users;"}),
                headers={"Content-Type": "application/json"},
                timeout=2,
            )
        except Exception:
            if time.time() >= deadline:
                raise
            time.sleep(0.1)
            continue

        if status == 503:
            assert payload["error_code"] == "QUEUE_FULL", payload
            break

        if time.time() >= deadline:
            raise SystemExit(f"expected 503 queue full, got {status} {payload}")
        time.sleep(0.1)
finally:
    for blocker in blockers:
        blocker.close()
PY

stop_server
printf '%s\n' "test_api_server.sh: ok"
