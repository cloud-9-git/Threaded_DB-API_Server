#!/bin/sh

set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
BIN_PATH="${MINI_DB_SERVER_BIN:-$ROOT_DIR/mini_db_server}"
RESULT_PATH="${RESULT_PATH:-$ROOT_DIR/build/bench_api_server_results.tsv}"
REQUESTS_PER_RUN="${REQUESTS_PER_RUN:-60}"
SEED_ROWS="${SEED_ROWS:-2000}"
TMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/mini-db-server-bench.XXXXXX")
SERVER_PID=""

cleanup() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" >/dev/null 2>&1 || true
        wait "$SERVER_PID" >/dev/null 2>&1 || true
    fi
    rm -rf "$TMP_DIR"
}

trap cleanup EXIT INT TERM

get_free_port() {
    python3 - <<'PY'
import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
PY
}

wait_for_server() {
    port=$1
    attempts=0
    while [ "$attempts" -lt 30 ]; do
        if curl -s "http://127.0.0.1:$port/health" >/dev/null 2>&1; then
            return 0
        fi
        attempts=$((attempts + 1))
        sleep 1
    done
    return 1
}

start_server() {
    db_dir=$1
    port=$2
    workers=$3
    queue_size=$4
    log_file=$5

    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" >/dev/null 2>&1 || true
        wait "$SERVER_PID" >/dev/null 2>&1 || true
        SERVER_PID=""
    fi

    "$BIN_PATH" -d "$db_dir" -p "$port" -t "$workers" -q "$queue_size" >"$log_file" 2>&1 &
    SERVER_PID=$!
    wait_for_server "$port"
}

seed_db() {
    db_dir=$1
    rows=$2
    python3 - "$db_dir" "$rows" <<'PY'
import pathlib
import sys

db_dir = pathlib.Path(sys.argv[1])
rows = int(sys.argv[2])
db_dir.mkdir(parents=True, exist_ok=True)
(db_dir / "users.schema").write_text("id\nname\nage\n", encoding="utf-8")
with (db_dir / "users.data").open("w", encoding="utf-8") as handle:
    for i in range(1, rows + 1):
        handle.write(f"{i}|user_{i}|{20 + (i % 50)}\n")
PY
}

empty_db() {
    db_dir=$1
    mkdir -p "$db_dir"
    cat > "$db_dir/users.schema" <<'EOF'
id
name
age
EOF
    : > "$db_dir/users.data"
}

emit_request() {
    workload=$1
    index=$2
    total_seed_rows=$3

    case "$workload" in
        select-only)
            probe_id=$(( (index % total_seed_rows) + 1 ))
            printf '{"sql":"SELECT * FROM users WHERE id = %s;"}' "$probe_id"
            ;;
        insert-only)
            printf '{"sql":"INSERT INTO users VALUES ('\''bench_user_%s'\'', %s);"}' "$index" $((20 + (index % 50)))
            ;;
        mixed)
            if [ $((index % 2)) -eq 0 ]; then
                probe_id=$(( (index % total_seed_rows) + 1 ))
                printf '{"sql":"SELECT * FROM users WHERE id = %s;"}' "$probe_id"
            else
                printf '{"sql":"INSERT INTO users VALUES ('\''bench_mix_%s'\'', %s);"}' "$index" $((20 + (index % 50)))
            fi
            ;;
        *)
            return 1
            ;;
    esac
}

run_batch() {
    base_url=$1
    workload=$2
    output_file=$3
    total_seed_rows=$4
    pids=""

    : > "$output_file"
    i=1
    while [ "$i" -le "$REQUESTS_PER_RUN" ]; do
        (
            payload=$(emit_request "$workload" "$i" "$total_seed_rows")
            if ! curl --max-time 20 -sS -o /dev/null \
                -w "%{http_code}\t%{time_total}\n" \
                -X POST "$base_url/query" \
                -H "Content-Type: application/json" \
                --data "$payload" >> "$output_file"; then
                printf '000\t20.000000\n' >> "$output_file"
            fi
        ) &
        pids="$pids $!"
        i=$((i + 1))
    done
    for pid in $pids; do
        wait "$pid"
    done
}

mkdir -p "$(dirname "$RESULT_PATH")"
printf 'workers\tqueue_size\tworkload\ttotal_requests\tsuccess_requests\trejected_requests\ttotal_ms\tavg_ms\tp95_ms\n' > "$RESULT_PATH"

for workers in 2 4 8; do
    for queue_size in 32 64 128; do
        for workload in select-only insert-only mixed; do
            DB_DIR="$TMP_DIR/${workers}_${queue_size}_${workload}_db"
            LOG_FILE="$TMP_DIR/${workers}_${queue_size}_${workload}.log"
            SAMPLE_FILE="$TMP_DIR/${workers}_${queue_size}_${workload}.tsv"
            PORT=$(get_free_port)
            BASE_URL="http://127.0.0.1:$PORT"

            case "$workload" in
                select-only)
                    seed_db "$DB_DIR" "$SEED_ROWS"
                    active_seed_rows="$SEED_ROWS"
                    ;;
                insert-only)
                    empty_db "$DB_DIR"
                    active_seed_rows=1
                    ;;
                mixed)
                    seed_db "$DB_DIR" "$SEED_ROWS"
                    active_seed_rows="$SEED_ROWS"
                    ;;
            esac

            START_MS=$(python3 - <<'PY'
import time
print(int(time.time() * 1000))
PY
)
            start_server "$DB_DIR" "$PORT" "$workers" "$queue_size" "$LOG_FILE"
            run_batch "$BASE_URL" "$workload" "$SAMPLE_FILE" "$active_seed_rows"
            END_MS=$(python3 - <<'PY'
import time
print(int(time.time() * 1000))
PY
)

            kill "$SERVER_PID" >/dev/null 2>&1 || true
            wait "$SERVER_PID" >/dev/null 2>&1 || true
            SERVER_PID=""

            python3 - "$workers" "$queue_size" "$workload" "$REQUESTS_PER_RUN" "$START_MS" "$END_MS" "$SAMPLE_FILE" >> "$RESULT_PATH" <<'PY'
import math
import statistics
import sys

workers = sys.argv[1]
queue_size = sys.argv[2]
workload = sys.argv[3]
total_requests = int(sys.argv[4])
start_ms = int(sys.argv[5])
end_ms = int(sys.argv[6])
sample_file = sys.argv[7]

statuses = []
latencies_ms = []
with open(sample_file, "r", encoding="utf-8") as handle:
    for line in handle:
        line = line.strip()
        if not line:
            continue
        status_text, time_text = line.split("\t", 1)
        statuses.append(int(status_text))
        latencies_ms.append(float(time_text) * 1000.0)

success_requests = sum(1 for status in statuses if status == 200)
rejected_requests = sum(1 for status in statuses if status == 503)
avg_ms = statistics.mean(latencies_ms) if latencies_ms else 0.0

if latencies_ms:
    ordered = sorted(latencies_ms)
    index = max(0, math.ceil(len(ordered) * 0.95) - 1)
    p95_ms = ordered[index]
else:
    p95_ms = 0.0

total_ms = float(end_ms - start_ms)
print(
    f"{workers}\t{queue_size}\t{workload}\t{total_requests}\t"
    f"{success_requests}\t{rejected_requests}\t{total_ms:.2f}\t{avg_ms:.2f}\t{p95_ms:.2f}"
)
PY
        done
    done
done

cat "$RESULT_PATH"
