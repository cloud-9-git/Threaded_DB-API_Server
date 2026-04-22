#!/usr/bin/env python3

import argparse
import concurrent.futures
import csv
import http.client
import json
import math
import shutil
import signal
import socket
import statistics
import subprocess
import tempfile
import threading
import time
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple


DEFAULT_DATASET_SIZES = [10_000, 100_000, 1_000_000]
DEFAULT_WORKERS = [2, 4, 8]
DEFAULT_QUEUES = [32, 64, 128]
DEFAULT_WORKLOADS = ["select-only", "insert-only", "mixed"]
DEFAULT_REQUESTS_BY_DATASET = {
    10_000: 3_000,
    100_000: 2_000,
    1_000_000: 1_000,
}
DEFAULT_CONCURRENCY = 128
DEFAULT_REPEATS = 10


@dataclass
class BenchmarkRow:
    repeat_index: int
    dataset_rows: int
    workers: int
    queue_size: int
    workload: str
    total_requests: int
    success_requests: int
    rejected_requests: int
    error_requests: int
    total_ms: float
    avg_ms: float
    p95_ms: float
    max_ms: float


@dataclass
class BenchmarkSummaryRow:
    dataset_rows: int
    workers: int
    queue_size: int
    workload: str
    total_requests: int
    repeats: int
    success_mean: float
    success_stddev: float
    rejected_mean: float
    rejected_stddev: float
    error_mean: float
    error_stddev: float
    total_ms_mean: float
    total_ms_stddev: float
    avg_ms_mean: float
    avg_ms_stddev: float
    p95_ms_mean: float
    p95_ms_stddev: float
    max_ms_mean: float
    max_ms_stddev: float


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run mini_db_server concurrency checks and benchmarks.")
    parser.add_argument("--server-bin", default="./mini_db_server")
    parser.add_argument("--root-dir", default=".")
    parser.add_argument("--dataset-sizes", nargs="+", type=int, default=DEFAULT_DATASET_SIZES)
    parser.add_argument("--workers", nargs="+", type=int, default=DEFAULT_WORKERS)
    parser.add_argument("--queues", nargs="+", type=int, default=DEFAULT_QUEUES)
    parser.add_argument("--workloads", nargs="+", default=DEFAULT_WORKLOADS)
    parser.add_argument("--concurrency", type=int, default=DEFAULT_CONCURRENCY)
    parser.add_argument("--repeats", type=int, default=DEFAULT_REPEATS)
    parser.add_argument("--csv-out", default="reports/data/api_server_benchmark_results.csv")
    parser.add_argument("--raw-csv-out", default="reports/data/api_server_benchmark_samples.csv")
    parser.add_argument("--report-out", default="reports/details/api_server_benchmark_report.md")
    parser.add_argument("--requests-10k", type=int, default=DEFAULT_REQUESTS_BY_DATASET[10_000])
    parser.add_argument("--requests-100k", type=int, default=DEFAULT_REQUESTS_BY_DATASET[100_000])
    parser.add_argument("--requests-1m", type=int, default=DEFAULT_REQUESTS_BY_DATASET[1_000_000])
    return parser.parse_args()


def request_budget_for_size(args: argparse.Namespace, dataset_rows: int) -> int:
    if dataset_rows == 10_000:
        return args.requests_10k
    if dataset_rows == 100_000:
        return args.requests_100k
    if dataset_rows == 1_000_000:
        return args.requests_1m
    return max(500, min(3_000, dataset_rows // 10))


def pick_free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def ensure_server_built(server_bin: Path, root_dir: Path) -> None:
    if server_bin.exists():
        return
    subprocess.run(["make", "mini_db_server"], cwd=root_dir, check=True)


def generate_dataset(base_dir: Path, dataset_rows: int) -> Path:
    dataset_dir = base_dir / f"dataset_{dataset_rows}"
    users_schema = dataset_dir / "users.schema"
    users_data = dataset_dir / "users.data"
    products_schema = dataset_dir / "products.schema"
    products_data = dataset_dir / "products.data"

    if dataset_dir.exists():
        return dataset_dir

    dataset_dir.mkdir(parents=True, exist_ok=True)
    users_schema.write_text("id\nname\nage\n", encoding="utf-8")
    products_schema.write_text("id\nname\nprice\n", encoding="utf-8")
    products_data.write_text("", encoding="utf-8")

    with users_data.open("w", encoding="utf-8") as handle:
        for row_id in range(1, dataset_rows + 1):
            handle.write(f"{row_id}|user_{row_id}|{20 + (row_id % 50)}\n")

    return dataset_dir


def copy_dataset(src_dir: Path) -> Path:
    tmp_dir = Path(tempfile.mkdtemp(prefix="mini_db_dataset_run_", dir="/tmp"))
    shutil.copy2(src_dir / "users.schema", tmp_dir / "users.schema")
    shutil.copy2(src_dir / "users.data", tmp_dir / "users.data")
    shutil.copy2(src_dir / "products.schema", tmp_dir / "products.schema")
    shutil.copy2(src_dir / "products.data", tmp_dir / "products.data")
    return tmp_dir


class ServerProcess:
    def __init__(self, server_bin: Path, db_dir: Path, workers: int, queue_size: int):
        self.server_bin = server_bin
        self.db_dir = db_dir
        self.workers = workers
        self.queue_size = queue_size
        self.port = pick_free_port()
        self.log_file = tempfile.NamedTemporaryFile(prefix="mini_db_server_", suffix=".log", delete=False)
        self.process = None

    def start(self) -> None:
        self.process = subprocess.Popen(
            [
                str(self.server_bin),
                "-d",
                str(self.db_dir),
                "-p",
                str(self.port),
                "-t",
                str(self.workers),
                "-q",
                str(self.queue_size),
            ],
            stdout=self.log_file,
            stderr=subprocess.STDOUT,
        )
        self.wait_until_healthy()

    def wait_until_healthy(self) -> None:
        deadline = time.time() + 20
        while time.time() < deadline:
            try:
                status, payload = http_json_request(self.port, "GET", "/health")
                if status == 200 and payload.get("success") is True:
                    return
            except Exception:
                pass
            time.sleep(0.1)
        raise RuntimeError(f"server did not become healthy on port {self.port}")

    def stop(self) -> None:
        if self.process is None:
            return
        self.process.send_signal(signal.SIGINT)
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=5)
        self.process = None
        self.log_file.close()

    def read_log_tail(self) -> str:
        with open(self.log_file.name, "r", encoding="utf-8", errors="replace") as handle:
            return handle.read()[-4000:]


def http_json_request(port: int,
                      method: str,
                      path: str,
                      payload: Optional[Dict[str, str]] = None,
                      timeout: float = 10.0) -> Tuple[int, Dict[str, object]]:
    headers = {}
    body = None
    if payload is not None:
        headers["Content-Type"] = "application/json"
        body = json.dumps(payload)

    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=timeout)
    conn.request(method, path, body=body, headers=headers)
    resp = conn.getresponse()
    raw = resp.read().decode("utf-8")
    status = resp.status
    conn.close()
    return status, json.loads(raw)


def warm_up_runtime(port: int, dataset_rows: int) -> None:
    status, payload = http_json_request(
        port,
        "POST",
        "/query",
        {"sql": f"SELECT * FROM users WHERE id = {min(dataset_rows, 1)};"},
    )
    if status != 200:
        raise RuntimeError(f"warmup failed: {status} {payload}")


def run_queue_full_check(server_bin: Path, base_dataset: Path) -> Dict[str, object]:
    db_dir = copy_dataset(base_dataset)
    server = ServerProcess(server_bin, db_dir, workers=1, queue_size=1)
    blockers: List[socket.socket] = []
    try:
        server.start()

        def open_blocker() -> socket.socket:
            sock = socket.create_connection(("127.0.0.1", server.port), timeout=5)
            sock.sendall(
                (
                    f"POST /query HTTP/1.1\r\n"
                    f"Host: 127.0.0.1:{server.port}\r\n"
                    "Content-Type: application/json\r\n"
                    "Content-Length: 100\r\n"
                    "\r\n"
                    '{"sql":"SELECT * FROM users WHERE id = 1;'
                ).encode("utf-8")
            )
            return sock

        blockers = [open_blocker(), open_blocker()]
        time.sleep(0.3)

        deadline = time.time() + 5
        last_result = None
        while time.time() < deadline:
            try:
                status, payload = http_json_request(
                    server.port,
                    "POST",
                    "/query",
                    {"sql": "SELECT * FROM users;"},
                    timeout=2,
                )
            except Exception as exc:
                last_result = {"status": 599, "payload": {"error": str(exc)}}
                time.sleep(0.1)
                continue

            last_result = {"status": status, "payload": payload}
            if status == 503 and payload.get("error_code") == "QUEUE_FULL":
                return {"passed": True, "status": status, "payload": payload}
            time.sleep(0.1)

        if last_result is None:
            return {"passed": False, "status": None, "payload": {"error": "no response"}}
        return {"passed": False, "status": last_result["status"], "payload": last_result["payload"]}
    finally:
        for blocker in blockers:
            blocker.close()
        server.stop()
        shutil.rmtree(db_dir, ignore_errors=True)


def run_concurrency_validation(server_bin: Path, dataset_dir: Path) -> Dict[str, object]:
    db_dir = copy_dataset(dataset_dir)
    server = ServerProcess(server_bin, db_dir, workers=8, queue_size=128)
    try:
        server.start()
        warm_up_runtime(server.port, 10_000)

        def select_task(_: int) -> Tuple[int, Dict[str, object]]:
            return http_json_request(server.port, "POST", "/query", {"sql": "SELECT * FROM users WHERE id = 42;"})

        with concurrent.futures.ThreadPoolExecutor(max_workers=64) as executor:
            select_results = list(executor.map(select_task, range(256)))

        select_ok = all(
            status == 200 and payload.get("used_index") is True and payload.get("row_count") == 1
            for status, payload in select_results
        )

        def insert_task(index: int) -> Tuple[int, Dict[str, object]]:
            return http_json_request(
                server.port,
                "POST",
                "/query",
                {"sql": f"INSERT INTO users (name, age) VALUES ('stress_{index}', '{20 + (index % 30)}');"},
            )

        with concurrent.futures.ThreadPoolExecutor(max_workers=64) as executor:
            insert_results = list(executor.map(insert_task, range(256)))

        generated_ids = [payload.get("generated_id") for status, payload in insert_results if status == 200]
        insert_ok = (
            len(generated_ids) == 256
            and len(set(generated_ids)) == 256
            and all(status == 200 for status, _ in insert_results)
        )

        status, payload = http_json_request(server.port, "POST", "/query", {"sql": "SELECT * FROM users;"}, timeout=20)
        expected_row_count = 10_000 + 256
        final_count_ok = status == 200 and payload.get("row_count") == expected_row_count

        return {
            "select_ok": select_ok,
            "insert_ok": insert_ok,
            "final_count_ok": final_count_ok,
            "generated_id_min": min(generated_ids) if generated_ids else None,
            "generated_id_max": max(generated_ids) if generated_ids else None,
            "final_row_count": payload.get("row_count") if isinstance(payload, dict) else None,
        }
    finally:
        server.stop()
        shutil.rmtree(db_dir, ignore_errors=True)


def build_sql(workload: str,
              request_index: int,
              dataset_rows: int,
              insert_counter: Dict[str, int],
              lock: threading.Lock) -> str:
    if workload == "select-only":
        row_id = (request_index % dataset_rows) + 1
        return f"SELECT * FROM users WHERE id = {row_id};"
    if workload == "insert-only":
        with lock:
            insert_counter["value"] += 1
            row_id = insert_counter["value"]
        return f"INSERT INTO users (name, age) VALUES ('bench_insert_{row_id}', '{20 + (row_id % 50)}');"
    if workload == "mixed":
        if request_index % 2 == 0:
            row_id = (request_index % dataset_rows) + 1
            return f"SELECT * FROM users WHERE id = {row_id};"
        with lock:
            insert_counter["value"] += 1
            row_id = insert_counter["value"]
        return f"INSERT INTO users (name, age) VALUES ('bench_mixed_{row_id}', '{20 + (row_id % 50)}');"
    raise ValueError(f"unsupported workload: {workload}")


def build_sql_batch(workload: str, dataset_rows: int, total_requests: int) -> List[str]:
    insert_counter = {"value": dataset_rows}
    insert_lock = threading.Lock()
    return [
        build_sql(workload, request_index, dataset_rows, insert_counter, insert_lock)
        for request_index in range(total_requests)
    ]


def run_single_benchmark(server_bin: Path,
                         base_dataset: Path,
                         dataset_rows: int,
                         workers: int,
                         queue_size: int,
                         workload: str,
                         total_requests: int,
                         concurrency: int,
                         repeat_index: int = 1) -> BenchmarkRow:
    db_dir = copy_dataset(base_dataset)
    server = ServerProcess(server_bin, db_dir, workers=workers, queue_size=queue_size)
    insert_counter = {"value": dataset_rows}
    insert_lock = threading.Lock()
    try:
        server.start()
        warm_up_runtime(server.port, dataset_rows)

        def run_one(request_index: int) -> Tuple[int, Dict[str, object], float]:
            sql = build_sql(workload, request_index, dataset_rows, insert_counter, insert_lock)
            started = time.perf_counter()
            try:
                status, payload = http_json_request(server.port, "POST", "/query", {"sql": sql}, timeout=20)
            except Exception as exc:
                elapsed_ms = (time.perf_counter() - started) * 1000.0
                return 599, {"error": str(exc)}, elapsed_ms
            elapsed_ms = (time.perf_counter() - started) * 1000.0
            return status, payload, elapsed_ms

        wall_started = time.perf_counter()
        with concurrent.futures.ThreadPoolExecutor(max_workers=concurrency) as executor:
            results = list(executor.map(run_one, range(total_requests)))
        wall_elapsed_ms = (time.perf_counter() - wall_started) * 1000.0

        latencies = [elapsed_ms for _, _, elapsed_ms in results]
        success_requests = sum(1 for status, _, _ in results if status == 200)
        rejected_requests = sum(1 for status, _, _ in results if status == 503)
        error_requests = total_requests - success_requests - rejected_requests
        p95_ms = sorted(latencies)[max(0, math.ceil(len(latencies) * 0.95) - 1)]

        return BenchmarkRow(
            repeat_index=repeat_index,
            dataset_rows=dataset_rows,
            workers=workers,
            queue_size=queue_size,
            workload=workload,
            total_requests=total_requests,
            success_requests=success_requests,
            rejected_requests=rejected_requests,
            error_requests=error_requests,
            total_ms=wall_elapsed_ms,
            avg_ms=statistics.fmean(latencies),
            p95_ms=p95_ms,
            max_ms=max(latencies),
        )
    finally:
        server.stop()
        shutil.rmtree(db_dir, ignore_errors=True)


def mean_stddev(values: Iterable[float]) -> Tuple[float, float]:
    materialized = list(values)
    if not materialized:
        return 0.0, 0.0
    mean_value = statistics.fmean(materialized)
    stddev_value = statistics.stdev(materialized) if len(materialized) > 1 else 0.0
    return mean_value, stddev_value


def aggregate_rows(samples: List[BenchmarkRow]) -> List[BenchmarkSummaryRow]:
    grouped: Dict[Tuple[int, int, int, str, int], List[BenchmarkRow]] = defaultdict(list)
    for sample in samples:
        key = (
            sample.dataset_rows,
            sample.workers,
            sample.queue_size,
            sample.workload,
            sample.total_requests,
        )
        grouped[key].append(sample)

    summaries: List[BenchmarkSummaryRow] = []
    for key in sorted(grouped):
        runs = grouped[key]
        success_mean, success_stddev = mean_stddev(run.success_requests for run in runs)
        rejected_mean, rejected_stddev = mean_stddev(run.rejected_requests for run in runs)
        error_mean, error_stddev = mean_stddev(run.error_requests for run in runs)
        total_ms_mean, total_ms_stddev = mean_stddev(run.total_ms for run in runs)
        avg_ms_mean, avg_ms_stddev = mean_stddev(run.avg_ms for run in runs)
        p95_ms_mean, p95_ms_stddev = mean_stddev(run.p95_ms for run in runs)
        max_ms_mean, max_ms_stddev = mean_stddev(run.max_ms for run in runs)
        summaries.append(
            BenchmarkSummaryRow(
                dataset_rows=key[0],
                workers=key[1],
                queue_size=key[2],
                workload=key[3],
                total_requests=key[4],
                repeats=len(runs),
                success_mean=success_mean,
                success_stddev=success_stddev,
                rejected_mean=rejected_mean,
                rejected_stddev=rejected_stddev,
                error_mean=error_mean,
                error_stddev=error_stddev,
                total_ms_mean=total_ms_mean,
                total_ms_stddev=total_ms_stddev,
                avg_ms_mean=avg_ms_mean,
                avg_ms_stddev=avg_ms_stddev,
                p95_ms_mean=p95_ms_mean,
                p95_ms_stddev=p95_ms_stddev,
                max_ms_mean=max_ms_mean,
                max_ms_stddev=max_ms_stddev,
            )
        )
    return summaries


def write_raw_csv(rows: Iterable[BenchmarkRow], csv_path: Path) -> None:
    csv_path.parent.mkdir(parents=True, exist_ok=True)
    with csv_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow([
            "repeat_index",
            "dataset_rows",
            "workers",
            "queue_size",
            "workload",
            "total_requests",
            "success_requests",
            "rejected_requests",
            "error_requests",
            "total_ms",
            "avg_ms",
            "p95_ms",
            "max_ms",
        ])
        for row in rows:
            writer.writerow([
                row.repeat_index,
                row.dataset_rows,
                row.workers,
                row.queue_size,
                row.workload,
                row.total_requests,
                row.success_requests,
                row.rejected_requests,
                row.error_requests,
                f"{row.total_ms:.3f}",
                f"{row.avg_ms:.3f}",
                f"{row.p95_ms:.3f}",
                f"{row.max_ms:.3f}",
            ])


def write_csv(rows: Iterable[BenchmarkSummaryRow], csv_path: Path) -> None:
    csv_path.parent.mkdir(parents=True, exist_ok=True)
    with csv_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow([
            "dataset_rows",
            "workers",
            "queue_size",
            "workload",
            "total_requests",
            "repeats",
            "success_mean",
            "success_stddev",
            "rejected_mean",
            "rejected_stddev",
            "error_mean",
            "error_stddev",
            "total_ms_mean",
            "total_ms_stddev",
            "avg_ms_mean",
            "avg_ms_stddev",
            "p95_ms_mean",
            "p95_ms_stddev",
            "max_ms_mean",
            "max_ms_stddev",
        ])
        for row in rows:
            writer.writerow([
                row.dataset_rows,
                row.workers,
                row.queue_size,
                row.workload,
                row.total_requests,
                row.repeats,
                f"{row.success_mean:.3f}",
                f"{row.success_stddev:.3f}",
                f"{row.rejected_mean:.3f}",
                f"{row.rejected_stddev:.3f}",
                f"{row.error_mean:.3f}",
                f"{row.error_stddev:.3f}",
                f"{row.total_ms_mean:.3f}",
                f"{row.total_ms_stddev:.3f}",
                f"{row.avg_ms_mean:.3f}",
                f"{row.avg_ms_stddev:.3f}",
                f"{row.p95_ms_mean:.3f}",
                f"{row.p95_ms_stddev:.3f}",
                f"{row.max_ms_mean:.3f}",
                f"{row.max_ms_stddev:.3f}",
            ])


def load_rows_from_csv(csv_path: Path) -> List[BenchmarkSummaryRow]:
    rows: List[BenchmarkSummaryRow] = []
    with csv_path.open("r", newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        for row in reader:
            rows.append(
                BenchmarkSummaryRow(
                    dataset_rows=int(row["dataset_rows"]),
                    workers=int(row["workers"]),
                    queue_size=int(row["queue_size"]),
                    workload=row["workload"],
                    total_requests=int(row["total_requests"]),
                    repeats=int(row["repeats"]),
                    success_mean=float(row["success_mean"]),
                    success_stddev=float(row["success_stddev"]),
                    rejected_mean=float(row["rejected_mean"]),
                    rejected_stddev=float(row["rejected_stddev"]),
                    error_mean=float(row["error_mean"]),
                    error_stddev=float(row["error_stddev"]),
                    total_ms_mean=float(row["total_ms_mean"]),
                    total_ms_stddev=float(row["total_ms_stddev"]),
                    avg_ms_mean=float(row["avg_ms_mean"]),
                    avg_ms_stddev=float(row["avg_ms_stddev"]),
                    p95_ms_mean=float(row["p95_ms_mean"]),
                    p95_ms_stddev=float(row["p95_ms_stddev"]),
                    max_ms_mean=float(row["max_ms_mean"]),
                    max_ms_stddev=float(row["max_ms_stddev"]),
                )
            )
    return rows


def choose_best_rows(rows: List[BenchmarkSummaryRow]) -> Dict[Tuple[int, str], BenchmarkSummaryRow]:
    best: Dict[Tuple[int, str], BenchmarkSummaryRow] = {}
    for row in rows:
        key = (row.dataset_rows, row.workload)
        if key not in best:
            best[key] = row
            continue

        current = best[key]
        current_failure_ratio = (current.rejected_mean + current.error_mean) / current.total_requests
        candidate_failure_ratio = (row.rejected_mean + row.error_mean) / row.total_requests

        if candidate_failure_ratio < current_failure_ratio:
            best[key] = row
            continue
        if candidate_failure_ratio > current_failure_ratio:
            continue

        if row.p95_ms_mean < current.p95_ms_mean:
            best[key] = row
            continue
        if row.p95_ms_mean == current.p95_ms_mean and (
            row.workers < current.workers or row.queue_size < current.queue_size
        ):
            best[key] = row
    return best


def format_workload_label(workload: str) -> str:
    labels = {
        "select-only": "조회 전용 (select-only)",
        "insert-only": "삽입 전용 (insert-only)",
        "mixed": "혼합 (mixed)",
    }
    return labels.get(workload, workload)


def format_mean_stddev(mean_value: float, stddev_value: float, decimals: int = 3) -> str:
    return f"{mean_value:.{decimals}f} +- {stddev_value:.{decimals}f}"


def append_reader_guide(lines: List[str], report_kind: str) -> None:
    lines.append("## 처음 보는 분을 위한 안내")
    lines.append("")
    if report_kind == "benchmark":
        lines.append("- 이 문서는 서버가 평소처럼 계속 요청을 받을 때 어떤 설정이 가장 무난한지 보는 보고서입니다.")
        lines.append("- 가장 먼저 볼 곳은 `최적 조합` 표입니다. 여기에는 각 데이터 규모에서 가장 안정적이었던 설정만 추려져 있습니다.")
    elif report_kind == "stress":
        lines.append("- 이 문서는 짧은 순간에 요청이 한꺼번에 몰릴 때 서버가 얼마나 잘 버티는지 보는 보고서입니다.")
        lines.append("- 가장 먼저 볼 곳은 `버스트 수준별 최적 조합` 표입니다. 여기에는 갑작스러운 몰림 상황에서 가장 안정적이었던 설정만 추려져 있습니다.")
    elif report_kind == "async":
        lines.append("- 이 문서는 수천 개 이상의 요청을 거의 동시에 보내는 극단 상황을 보는 보고서입니다.")
        lines.append("- 가장 먼저 볼 곳은 `버스트 수준별 최적 조합` 표입니다. 여기에는 매우 큰 동시 접속에서 상대적으로 가장 나은 설정만 추려져 있습니다.")
    lines.append("- `워커 수`는 동시에 실제 일을 처리하는 인원 수, `큐 크기`는 잠시 대기시켜 둘 수 있는 줄 길이라고 생각하면 이해하기 쉽습니다.")
    lines.append("- 좋은 조합은 보통 `성공`이 높고, `503`과 `오류`가 낮고, `p95 ms`와 `표준편차`가 작은 조합입니다.")
    lines.append("")
    lines.append("## 용어 설명")
    lines.append("")
    lines.append("- `성공`: 요청이 정상적으로 처리된 횟수입니다. 높을수록 좋습니다.")
    lines.append("- `503`: 서버가 너무 바빠서 이번 요청은 받지 못하겠다고 돌려준 횟수입니다. 큐가 꽉 찼을 때 주로 나옵니다.")
    lines.append("- `오류`: 정상 응답(JSON) 자체를 받지 못한 횟수입니다. 연결 실패나 클라이언트/OS 한계까지 포함될 수 있습니다.")
    lines.append("- `평균 ms`: 전체 요청을 평균적으로 얼마나 빨리 처리했는지 보여줍니다.")
    lines.append("- `p95 ms`: 느린 요청들까지 포함했을 때도 얼마나 괜찮은지 보여줍니다. 체감 품질을 볼 때 평균보다 더 중요합니다.")
    lines.append("- `표준편차`: 같은 실험을 여러 번 반복했을 때 결과가 얼마나 흔들렸는지 나타냅니다. 작을수록 더 안정적입니다.")
    lines.append("")
    lines.append("## 표 읽는 법")
    lines.append("")
    lines.append("- `성공`이 높아도 `p95 ms`와 `표준편차`가 크면 실제 운영에서는 가끔씩 느려질 수 있습니다.")
    lines.append("- 그래서 이 보고서는 `실패율 -> p95 지연 -> 표준편차 -> 더 작은 자원 사용` 순서로 조합을 비교합니다.")
    if report_kind == "async":
        lines.append("- async 리포트의 `20k` 구간은 서버 한계뿐 아니라, 테스트를 돌린 로컬 컴퓨터의 포트 한계도 같이 섞여 해석해야 합니다.")
    lines.append("")


def append_full_results_section(lines: List[str],
                                rows: List[BenchmarkSummaryRow],
                                dataset_sizes: List[int],
                                workloads: List[str]) -> None:
    lines.append("## 전체 결과")
    lines.append("")
    for dataset_rows in dataset_sizes:
        lines.append(f"### 데이터셋 {dataset_rows}행")
        lines.append("")
        for workload in workloads:
            subset = [
                row for row in rows
                if row.dataset_rows == dataset_rows and row.workload == workload
            ]
            subset.sort(key=lambda row: (row.workers, row.queue_size))
            lines.append(f"#### {format_workload_label(workload)}")
            lines.append("")
            lines.append("| 워커 수 | 큐 크기 | 총 요청 수 | 반복 수 | 성공 평균+-표준편차 | 503 평균+-표준편차 | 오류 평균+-표준편차 | 평균 ms 평균+-표준편차 | p95 ms 평균+-표준편차 | 최대 ms 평균+-표준편차 |")
            lines.append("| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |")
            for row in subset:
                lines.append(
                    f"| {row.workers} | {row.queue_size} | {row.total_requests} | {row.repeats} | "
                    f"{format_mean_stddev(row.success_mean, row.success_stddev, 1)} | "
                    f"{format_mean_stddev(row.rejected_mean, row.rejected_stddev, 1)} | "
                    f"{format_mean_stddev(row.error_mean, row.error_stddev, 1)} | "
                    f"{format_mean_stddev(row.avg_ms_mean, row.avg_ms_stddev)} | "
                    f"{format_mean_stddev(row.p95_ms_mean, row.p95_ms_stddev)} | "
                    f"{format_mean_stddev(row.max_ms_mean, row.max_ms_stddev)} |"
                )
            lines.append("")


def build_report(rows: List[BenchmarkSummaryRow],
                 csv_path: Path,
                 raw_csv_path: Path,
                 report_path: Path,
                 concurrency_result: Dict[str, object],
                 queue_full_result: Dict[str, object],
                 args: argparse.Namespace) -> None:
    report_path.parent.mkdir(parents=True, exist_ok=True)
    best_rows = choose_best_rows(rows)
    lines: List[str] = []
    lines.append("# mini_db_server 벤치마크 리포트")
    lines.append("")
    append_reader_guide(lines, "benchmark")
    lines.append("## 측정 조건")
    lines.append("")
    lines.append(f"- 서버 바이너리: `{args.server_bin}`")
    lines.append(f"- 데이터셋 크기: `{', '.join(str(size) for size in args.dataset_sizes)}`행")
    lines.append(f"- 워커 수 후보: `{', '.join(str(worker) for worker in args.workers)}`")
    lines.append(f"- 큐 크기 후보: `{', '.join(str(queue) for queue in args.queues)}`")
    lines.append(f"- 부하 종류: `{', '.join(format_workload_label(workload) for workload in args.workloads)}`")
    lines.append(f"- 클라이언트 동시성: `{args.concurrency}`")
    lines.append(f"- 반복 횟수: 각 조합당 `{args.repeats}`회")
    lines.append(f"- 데이터셋별 요청 수: `10k={args.requests_10k}`, `100k={args.requests_100k}`, `1M={args.requests_1m}`")
    lines.append(f"- 요약 CSV 결과 파일: `{csv_path}`")
    lines.append(f"- raw 샘플 CSV 결과 파일: `{raw_csv_path}`")
    lines.append("")
    lines.append("## 동시성 검증")
    lines.append("")
    lines.append(f"- 동시 SELECT 정합성: `{'통과' if concurrency_result['select_ok'] else '실패'}`")
    lines.append(f"- 동시 INSERT 고유성: `{'통과' if concurrency_result['insert_ok'] else '실패'}`")
    lines.append(f"- 256개 동시 INSERT 이후 최종 row count: `{'통과' if concurrency_result['final_count_ok'] else '실패'}`")
    lines.append(f"- 동시 INSERT 중 생성된 id 범위: `{concurrency_result['generated_id_min']}..{concurrency_result['generated_id_max']}`")
    lines.append(f"- Queue full 검증: `{'통과' if queue_full_result['passed'] else '실패'}`")
    if not queue_full_result["passed"]:
        lines.append(f"- Queue full 관측 결과: `{queue_full_result}`")
    lines.append("")
    lines.append("## 최적 조합")
    lines.append("")
    lines.append("| 데이터셋 행 수 | 부하 종류 | 최적 워커/큐 | 성공 평균+-표준편차 | 503 평균+-표준편차 | 평균 ms 평균+-표준편차 | p95 ms 평균+-표준편차 |")
    lines.append("| --- | --- | --- | --- | --- | --- | --- |")
    for key in sorted(best_rows):
        row = best_rows[key]
        lines.append(
            f"| {row.dataset_rows} | {format_workload_label(row.workload)} | {row.workers}/{row.queue_size} | "
            f"{format_mean_stddev(row.success_mean, row.success_stddev, 1)} / {row.total_requests} | "
            f"{format_mean_stddev(row.rejected_mean, row.rejected_stddev, 1)} | "
            f"{format_mean_stddev(row.avg_ms_mean, row.avg_ms_stddev)} | "
            f"{format_mean_stddev(row.p95_ms_mean, row.p95_ms_stddev)} |"
        )
    lines.append("")
    lines.append("## 해석")
    lines.append("")
    lines.append("- 측정 전 인덱스를 사용하는 SELECT 1회를 먼저 보내 워밍업했기 때문에, 본 측정 지연 시간에는 최초 인덱스 생성 비용이 포함되지 않습니다.")
    lines.append("- 각 조합을 10회 반복 측정했고, 표의 지연 시간과 성공/실패 수는 모두 평균과 표준편차를 같이 표시합니다.")
    lines.append("- `503` 개수는 선택한 클라이언트 동시성에서 과부하가 걸렸을 때 반환된 응답 수입니다. 별도의 queue-full 검증으로 bounded queue 거절 경로도 따로 확인했습니다.")
    lines.append("- 거절과 오류의 합이 같으면 p95 지연 시간이 더 낮은 조합을 우선 선택했고, 그마저 비슷하면 더 작은 워커/큐 조합을 우선 선택했습니다.")
    lines.append("")
    append_full_results_section(lines, rows, args.dataset_sizes, args.workloads)
    report_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> None:
    args = parse_args()
    root_dir = Path(args.root_dir).resolve()
    server_bin = (root_dir / args.server_bin).resolve() if not Path(args.server_bin).is_absolute() else Path(args.server_bin)
    csv_path = (root_dir / args.csv_out).resolve()
    raw_csv_path = (root_dir / args.raw_csv_out).resolve()
    report_path = (root_dir / args.report_out).resolve()

    ensure_server_built(server_bin, root_dir)

    datasets_root = Path(tempfile.mkdtemp(prefix="mini_db_datasets_", dir="/tmp"))
    samples: List[BenchmarkRow] = []
    try:
        dataset_dirs = {size: generate_dataset(datasets_root, size) for size in args.dataset_sizes}
        validation_dataset = dataset_dirs.get(10_000)
        if validation_dataset is None:
            validation_dataset = generate_dataset(datasets_root, 10_000)

        concurrency_result = run_concurrency_validation(server_bin, validation_dataset)
        queue_full_result = run_queue_full_check(server_bin, validation_dataset)

        for dataset_rows in args.dataset_sizes:
            for workers in args.workers:
                for queue_size in args.queues:
                    for workload in args.workloads:
                        total_requests = request_budget_for_size(args, dataset_rows)
                        for repeat_index in range(1, args.repeats + 1):
                            row = run_single_benchmark(
                                server_bin=server_bin,
                                base_dataset=dataset_dirs[dataset_rows],
                                dataset_rows=dataset_rows,
                                workers=workers,
                                queue_size=queue_size,
                                workload=workload,
                                total_requests=total_requests,
                                concurrency=args.concurrency,
                                repeat_index=repeat_index,
                            )
                            samples.append(row)
                            print(
                                f"[bench] rows={row.dataset_rows} workers={row.workers} queue={row.queue_size} "
                                f"workload={row.workload} repeat={row.repeat_index}/{args.repeats} "
                                f"success={row.success_requests}/{row.total_requests} "
                                f"503={row.rejected_requests} errors={row.error_requests} "
                                f"avg_ms={row.avg_ms:.3f} p95_ms={row.p95_ms:.3f}",
                                flush=True,
                            )

        summary_rows = aggregate_rows(samples)
        write_raw_csv(samples, raw_csv_path)
        write_csv(summary_rows, csv_path)
        build_report(summary_rows, csv_path, raw_csv_path, report_path, concurrency_result, queue_full_result, args)
        print(f"[done] wrote summary CSV to {csv_path}")
        print(f"[done] wrote raw CSV to {raw_csv_path}")
        print(f"[done] wrote report to {report_path}")
    finally:
        shutil.rmtree(datasets_root, ignore_errors=True)


if __name__ == "__main__":
    main()
