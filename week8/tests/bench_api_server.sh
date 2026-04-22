#!/bin/sh

set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
BIN_PATH="${MINI_DB_SERVER_BIN:-$ROOT_DIR/mini_db_server}"
OUT_FILE="${OUT_FILE:-$ROOT_DIR/build/api_benchmark.csv}"
TOTAL_REQUESTS="${TOTAL_REQUESTS:-200}"
CONCURRENCY="${CONCURRENCY:-32}"
WORKERS_LIST="${WORKERS_LIST:-2 4 8}"
QUEUE_LIST="${QUEUE_LIST:-32 64 128}"
WORKLOADS="${WORKLOADS:-select-only insert-only mixed}"
BASE_PORT="${BASE_PORT:-19080}"

mkdir -p "$(dirname "$OUT_FILE")"

python3 - "$BIN_PATH" "$OUT_FILE" "$TOTAL_REQUESTS" "$CONCURRENCY" "$WORKERS_LIST" "$QUEUE_LIST" "$WORKLOADS" "$BASE_PORT" <<'PY'
import csv
import http.client
import json
import os
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor

bin_path, out_file = sys.argv[1], sys.argv[2]
total_requests = int(sys.argv[3])
concurrency = int(sys.argv[4])
workers_list = [int(x) for x in sys.argv[5].split()]
queue_list = [int(x) for x in sys.argv[6].split()]
workloads = sys.argv[7].split()
base_port = int(sys.argv[8])

def write_schema(db_dir):
    os.makedirs(db_dir, exist_ok=True)
    with open(os.path.join(db_dir, "users.schema"), "w", encoding="utf-8") as f:
        f.write("id\nname\nage\n")

def request(port, sql):
    start = time.perf_counter()
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    body = json.dumps({"sql": sql})
    try:
        conn.request("POST", "/query", body=body, headers={"Content-Type": "application/json"})
        resp = conn.getresponse()
        resp.read()
        status = resp.status
    except Exception:
        status = 0
    finally:
        conn.close()
    return status, (time.perf_counter() - start) * 1000.0

def wait_ready(port):
    deadline = time.time() + 5
    while time.time() < deadline:
        try:
            conn = http.client.HTTPConnection("127.0.0.1", port, timeout=1)
            conn.request("GET", "/health")
            resp = conn.getresponse()
            resp.read()
            conn.close()
            if resp.status == 200:
                return
        except Exception:
            time.sleep(0.05)
    raise RuntimeError("server did not become ready")

def sql_for(workload, index):
    if workload == "select-only":
        return "SELECT * FROM users WHERE id = 1;"
    if workload == "insert-only":
        return "INSERT INTO users VALUES ('bench%d', %d);" % (index, 20 + (index % 50))
    if index % 2 == 0:
        return "SELECT * FROM users WHERE id = 1;"
    return "INSERT INTO users VALUES ('mixed%d', %d);" % (index, 20 + (index % 50))

def run_case(workers, queue_size, workload, port):
    tmp = tempfile.mkdtemp(prefix="mini-db-bench.")
    db_dir = os.path.join(tmp, "db")
    write_schema(db_dir)
    proc = subprocess.Popen(
        [bin_path, "-d", db_dir, "-p", str(port), "-t", str(workers), "-q", str(queue_size)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    try:
        wait_ready(port)
        seed_status, _ = request(port, "INSERT INTO users VALUES ('seed', 1);")
        if seed_status != 200:
            raise RuntimeError("seed insert failed")

        started = time.perf_counter()
        with ThreadPoolExecutor(max_workers=concurrency) as executor:
            results = list(executor.map(lambda i: request(port, sql_for(workload, i)), range(total_requests)))
        total_ms = (time.perf_counter() - started) * 1000.0
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=2)
        shutil.rmtree(tmp, ignore_errors=True)

    statuses = [status for status, _ in results]
    latencies = [latency for _, latency in results]
    success = sum(1 for status in statuses if status == 200)
    rejected = sum(1 for status in statuses if status == 503)
    failures = total_requests - success
    avg_ms = statistics.mean(latencies) if latencies else 0.0
    p95_ms = sorted(latencies)[int(len(latencies) * 0.95) - 1] if latencies else 0.0

    return {
        "workers": workers,
        "queue_size": queue_size,
        "workload": workload,
        "total_requests": total_requests,
        "success_requests": success,
        "rejected_requests": rejected,
        "failure_requests": failures,
        "total_ms": round(total_ms, 2),
        "avg_ms": round(avg_ms, 2),
        "p95_ms": round(p95_ms, 2),
    }

rows = []
case_index = 0
for workers in workers_list:
    for queue_size in queue_list:
        for workload in workloads:
            port = base_port + case_index
            case_index += 1
            row = run_case(workers, queue_size, workload, port)
            rows.append(row)
            print(
                "workers=%s queue=%s workload=%s success=%s rejected=%s total_ms=%s avg_ms=%s p95_ms=%s"
                % (
                    row["workers"],
                    row["queue_size"],
                    row["workload"],
                    row["success_requests"],
                    row["rejected_requests"],
                    row["total_ms"],
                    row["avg_ms"],
                    row["p95_ms"],
                )
            )

fields = [
    "workers",
    "queue_size",
    "workload",
    "total_requests",
    "success_requests",
    "rejected_requests",
    "failure_requests",
    "total_ms",
    "avg_ms",
    "p95_ms",
]
with open(out_file, "w", newline="", encoding="utf-8") as f:
    writer = csv.DictWriter(f, fieldnames=fields)
    writer.writeheader()
    writer.writerows(rows)

mixed = [row for row in rows if row["workload"] == "mixed" and row["rejected_requests"] == 0]
if mixed:
    best = min(mixed, key=lambda row: (row["p95_ms"], row["avg_ms"], row["workers"], row["queue_size"]))
    print(
        "recommended_default workers=%s queue_size=%s workload=mixed p95_ms=%s avg_ms=%s"
        % (best["workers"], best["queue_size"], best["p95_ms"], best["avg_ms"])
    )
print("wrote %s" % out_file)
PY
