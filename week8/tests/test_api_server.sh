#!/bin/sh

set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
BIN_PATH="${MINI_DB_SERVER_BIN:-$ROOT_DIR/mini_db_server}"
TMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/mini-db-api.XXXXXX")
PORT="${API_TEST_PORT:-18080}"
QUEUE_PORT="${API_QUEUE_TEST_PORT:-18081}"
SERVER_PID=""
QUEUE_SERVER_PID=""

cleanup() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    if [ -n "$QUEUE_SERVER_PID" ]; then
        kill "$QUEUE_SERVER_PID" 2>/dev/null || true
        wait "$QUEUE_SERVER_PID" 2>/dev/null || true
    fi
    rm -rf "$TMP_DIR"
}

trap 'status=$?; cleanup; exit $status' EXIT INT TERM

mkdir -p "$TMP_DIR/db" "$TMP_DIR/queue-db"
printf 'id\nname\nage\n' > "$TMP_DIR/db/users.schema"
printf 'id\nname\nage\n' > "$TMP_DIR/queue-db/users.schema"

"$BIN_PATH" -d "$TMP_DIR/db" -p "$PORT" -t 4 -q 16 > "$TMP_DIR/server.log" 2>&1 &
SERVER_PID=$!

python3 - "$PORT" <<'PY'
import http.client
import json
import sys
import time
from concurrent.futures import ThreadPoolExecutor

port = int(sys.argv[1])

def request(method, path, body=None):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    headers = {}
    if body is not None:
        headers["Content-Type"] = "application/json"
    conn.request(method, path, body=body, headers=headers)
    resp = conn.getresponse()
    data = resp.read().decode("utf-8")
    conn.close()
    return resp.status, data

deadline = time.time() + 5
while True:
    try:
        status, body = request("GET", "/health")
        if status == 200 and json.loads(body)["service"] == "mini_db_server":
            break
    except Exception:
        pass
    if time.time() > deadline:
        raise SystemExit("server did not become ready")
    time.sleep(0.05)

status, body = request("POST", "/query", "{")
assert status == 400, (status, body)
assert json.loads(body)["error_code"] == "INVALID_JSON"

status, body = request("POST", "/query", json.dumps({"sql": "INSERT INTO users VALUES ('kim', 25);"}))
doc = json.loads(body)
assert status == 200, (status, body)
assert doc["success"] is True
assert doc["type"] == "insert"
assert doc["affected_rows"] == 1
assert doc["generated_id"] == 1

status, body = request("POST", "/query", json.dumps({"sql": "SELECT * FROM users WHERE id = 1;"}))
doc = json.loads(body)
assert status == 200, (status, body)
assert doc == {
    "success": True,
    "type": "select",
    "used_index": True,
    "row_count": 1,
    "columns": ["id", "name", "age"],
    "rows": [["1", "kim", "25"]],
}

status, body = request("POST", "/query", json.dumps({"sql": "SELECT * FROM users; SELECT * FROM users;"}))
assert status == 400, (status, body)
assert json.loads(body)["error_code"] == "MULTI_STATEMENT_NOT_ALLOWED"

def concurrent_select(_):
    status, body = request("POST", "/query", json.dumps({"sql": "SELECT * FROM users WHERE id = 1;"}))
    doc = json.loads(body)
    return status == 200 and doc["used_index"] is True and doc["row_count"] == 1

with ThreadPoolExecutor(max_workers=16) as executor:
    assert all(executor.map(concurrent_select, range(32)))

def concurrent_insert(i):
    sql = "INSERT INTO users VALUES ('u%d', %d);" % (i, 30 + i)
    status, body = request("POST", "/query", json.dumps({"sql": sql}))
    doc = json.loads(body)
    return status, doc.get("generated_id")

with ThreadPoolExecutor(max_workers=8) as executor:
    results = list(executor.map(concurrent_insert, range(16)))
assert all(status == 200 for status, _ in results), results
assert len({generated_id for _, generated_id in results}) == 16

print("api functional checks: OK")
PY

"$BIN_PATH" -d "$TMP_DIR/queue-db" -p "$QUEUE_PORT" -t 1 -q 1 > "$TMP_DIR/queue-server.log" 2>&1 &
QUEUE_SERVER_PID=$!

python3 - "$QUEUE_PORT" <<'PY'
import socket
import sys
import time

port = int(sys.argv[1])

def wait_ready():
    deadline = time.time() + 5
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=1) as sock:
                sock.sendall(b"GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n")
                if b"200 OK" in sock.recv(256):
                    return
        except OSError:
            time.sleep(0.05)
    raise SystemExit("queue test server did not become ready")

wait_ready()

slow1 = socket.create_connection(("127.0.0.1", port), timeout=2)
slow1.sendall(b"POST /query HTTP/1.1\r\nHost: localhost\r\nContent-Length: 10\r\n\r\n")
time.sleep(0.2)

slow2 = socket.create_connection(("127.0.0.1", port), timeout=2)
slow2.sendall(b"POST /query HTTP/1.1\r\nHost: localhost\r\nContent-Length: 10\r\n\r\n")
time.sleep(0.2)

fast = socket.create_connection(("127.0.0.1", port), timeout=2)
fast.sendall(b"GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n")
chunks = []
while True:
    try:
        chunk = fast.recv(4096)
    except ConnectionResetError:
        break
    if not chunk:
        break
    chunks.append(chunk)
response = b"".join(chunks)
fast.close()
slow1.close()
slow2.close()

assert b"503 Service Unavailable" in response, response
assert b"QUEUE_FULL" in response, response
print("queue full check: OK")
PY

echo "test_api_server: OK"
