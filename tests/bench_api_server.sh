#!/bin/sh

set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
DB_SOURCE=${DB_SOURCE:-"$ROOT_DIR/db"}
OUTPUT_FILE=${OUTPUT_FILE:-"$ROOT_DIR/tests/bench_api_server_results.csv"}
WORKERS_LIST=${WORKERS_LIST:-"2 4 8"}
QUEUE_LIST=${QUEUE_LIST:-"32 64 128"}
WORKLOADS=${WORKLOADS:-"select-only insert-only mixed"}
TOTAL_REQUESTS=${TOTAL_REQUESTS:-120}
CONCURRENCY=${CONCURRENCY:-16}
SERVER_PID=""
BENCH_TMP_DB=""
BENCH_LOG=""

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
    if [ -n "$BENCH_TMP_DB" ]; then
        rm -rf "$BENCH_TMP_DB"
    fi
    if [ -n "$BENCH_LOG" ]; then
        rm -f "$BENCH_LOG"
    fi
}

start_server() {
    PORT="$1"
    THREADS="$2"
    QUEUE_SIZE="$3"

    "$ROOT_DIR/mini_db_server" -d "$BENCH_TMP_DB" -p "$PORT" -t "$THREADS" -q "$QUEUE_SIZE" >"$BENCH_LOG" 2>&1 &
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

mkdir -p "$(dirname "$OUTPUT_FILE")"
printf '%s\n' "workers,queue_size,workload,total_requests,success_requests,rejected_requests,total_ms,avg_ms,p95_ms" >"$OUTPUT_FILE"

for workers in $WORKERS_LIST; do
    for queue_size in $QUEUE_LIST; do
        for workload in $WORKLOADS; do
            BENCH_TMP_DB=$(mktemp -d /tmp/mini_db_server_bench.XXXXXX)
            BENCH_LOG=$(mktemp /tmp/mini_db_server_bench_log.XXXXXX)
            cp "$DB_SOURCE"/*.schema "$BENCH_TMP_DB"/
            cp "$DB_SOURCE"/*.data "$BENCH_TMP_DB"/

            PORT=$(pick_free_port)
            start_server "$PORT" "$workers" "$queue_size"

            RESULT_LINE=$(PORT="$PORT" WORKLOAD="$workload" TOTAL_REQUESTS="$TOTAL_REQUESTS" CONCURRENCY="$CONCURRENCY" python3 - <<'PY'
import concurrent.futures
import http.client
import json
import math
import os
import threading
import time

port = int(os.environ["PORT"])
workload = os.environ["WORKLOAD"]
total_requests = int(os.environ["TOTAL_REQUESTS"])
concurrency = int(os.environ["CONCURRENCY"])
preload_rows = 50


def request(method, path, body=None, headers=None, timeout=5):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=timeout)
    conn.request(method, path, body=body, headers=headers or {})
    resp = conn.getresponse()
    text = resp.read().decode("utf-8")
    status = resp.status
    conn.close()
    payload = json.loads(text)
    return status, payload


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
        if status == 200 and payload["success"] is True:
            break
    except Exception:
        pass

    if time.time() >= deadline:
        raise SystemExit("server did not become healthy")
    time.sleep(0.1)

for index in range(preload_rows):
    status, payload = query(f"INSERT INTO users (name, age) VALUES ('seed_{index}', '{20 + (index % 10)}');")
    if status != 200:
        raise SystemExit(f"failed to preload data: {status} {payload}")

counter = {"value": preload_rows}
counter_lock = threading.Lock()


def build_sql(request_index):
    if workload == "select-only":
        row_id = (request_index % preload_rows) + 1
        return f"SELECT * FROM users WHERE id = {row_id};"

    if workload == "insert-only":
        with counter_lock:
            counter["value"] += 1
            row_id = counter["value"]
        return f"INSERT INTO users (name, age) VALUES ('bench_insert_{row_id}', '{20 + (row_id % 30)}');"

    if workload == "mixed":
        if request_index % 2 == 0:
            row_id = (request_index % preload_rows) + 1
            return f"SELECT * FROM users WHERE id = {row_id};"
        with counter_lock:
            counter["value"] += 1
            row_id = counter["value"]
        return f"INSERT INTO users (name, age) VALUES ('bench_mixed_{row_id}', '{20 + (row_id % 30)}');"

    raise SystemExit(f"unsupported workload: {workload}")


def run_one(request_index):
    sql = build_sql(request_index)
    started = time.perf_counter()
    status, payload = query(sql)
    elapsed_ms = (time.perf_counter() - started) * 1000.0
    return status, payload, elapsed_ms


wall_started = time.perf_counter()
with concurrent.futures.ThreadPoolExecutor(max_workers=concurrency) as executor:
    results = list(executor.map(run_one, range(total_requests)))
wall_elapsed_ms = (time.perf_counter() - wall_started) * 1000.0

latencies = [elapsed_ms for _, _, elapsed_ms in results]
success_count = sum(1 for status, _, _ in results if status == 200)
rejected_count = sum(1 for status, _, _ in results if status == 503)
latencies_sorted = sorted(latencies)
p95_index = max(0, math.ceil(len(latencies_sorted) * 0.95) - 1)
p95_ms = latencies_sorted[p95_index]
avg_ms = sum(latencies) / len(latencies)

print(
    f"{total_requests},{success_count},{rejected_count},{wall_elapsed_ms:.3f},{avg_ms:.3f},{p95_ms:.3f}"
)
PY
)

            stop_server

            IFS=, read -r total_requests success_requests rejected_requests total_ms avg_ms p95_ms <<EOF
$RESULT_LINE
EOF

            printf '%s\n' "$workers,$queue_size,$workload,$total_requests,$success_requests,$rejected_requests,$total_ms,$avg_ms,$p95_ms" >>"$OUTPUT_FILE"
            printf '%s\n' "bench_api_server.sh: workers=$workers queue=$queue_size workload=$workload total=$total_requests success=$success_requests rejected=$rejected_requests avg_ms=$avg_ms p95_ms=$p95_ms"

            rm -rf "$BENCH_TMP_DB"
            rm -f "$BENCH_LOG"
            BENCH_TMP_DB=""
            BENCH_LOG=""
        done
    done
done

printf '%s\n' "bench_api_server.sh: wrote $OUTPUT_FILE"
