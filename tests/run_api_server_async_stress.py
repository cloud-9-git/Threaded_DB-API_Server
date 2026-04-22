#!/usr/bin/env python3

import argparse
import asyncio
import csv
import json
import shutil
import tempfile
import time
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Tuple

from run_api_server_benchmarks import (
    BenchmarkRow,
    BenchmarkSummaryRow,
    ServerProcess,
    aggregate_rows,
    append_reader_guide,
    build_sql_batch,
    ensure_server_built,
    format_mean_stddev,
    format_workload_label,
    generate_dataset,
    warm_up_runtime,
    write_csv,
)


DEFAULT_DATASET_SIZES = [100_000, 1_000_000]
DEFAULT_WORKERS = [16]
DEFAULT_QUEUES = [512]
DEFAULT_WORKLOADS = ["mixed"]
DEFAULT_CONCURRENCY_LEVELS = [2_000, 5_000, 10_000, 20_000]
DEFAULT_REPEATS = 10
DEFAULT_TIMEOUT_SECONDS = 60.0
DEFAULT_STARTUP_RETRIES = 5
DEFAULT_STARTUP_RETRY_DELAY_SECONDS = 3.0
DEFAULT_COOLDOWN_SECONDS = 2.0


@dataclass
class AsyncSample:
    row: BenchmarkRow
    error_breakdown: Counter


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run asyncio-based high-concurrency burst tests for mini_db_server.")
    parser.add_argument("--server-bin", default="./mini_db_server")
    parser.add_argument("--root-dir", default=".")
    parser.add_argument("--dataset-sizes", nargs="+", type=int, default=DEFAULT_DATASET_SIZES)
    parser.add_argument("--workers", nargs="+", type=int, default=DEFAULT_WORKERS)
    parser.add_argument("--queues", nargs="+", type=int, default=DEFAULT_QUEUES)
    parser.add_argument("--workloads", nargs="+", default=DEFAULT_WORKLOADS)
    parser.add_argument("--concurrency-levels", nargs="+", type=int, default=DEFAULT_CONCURRENCY_LEVELS)
    parser.add_argument("--repeats", type=int, default=DEFAULT_REPEATS)
    parser.add_argument("--timeout-seconds", type=float, default=DEFAULT_TIMEOUT_SECONDS)
    parser.add_argument("--startup-retries", type=int, default=DEFAULT_STARTUP_RETRIES)
    parser.add_argument("--startup-retry-delay-seconds", type=float, default=DEFAULT_STARTUP_RETRY_DELAY_SECONDS)
    parser.add_argument("--cooldown-seconds", type=float, default=DEFAULT_COOLDOWN_SECONDS)
    parser.add_argument("--csv-out", default="reports/data/api_server_async_stress_results.csv")
    parser.add_argument("--raw-csv-out", default="reports/data/api_server_async_stress_samples.csv")
    parser.add_argument("--report-out", default="reports/details/api_server_async_stress_report.md")
    return parser.parse_args()


def choose_best_rows(rows: List[BenchmarkSummaryRow]):
    best = {}
    for row in rows:
        key = (row.dataset_rows, row.workload, row.total_requests)
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


def format_error_label(exc: Exception) -> str:
    if isinstance(exc, asyncio.TimeoutError):
        return "timeout"
    if isinstance(exc, ConnectionResetError):
        return "connection_reset"
    if isinstance(exc, ConnectionRefusedError):
        return "connection_refused"
    if isinstance(exc, BrokenPipeError):
        return "broken_pipe"
    if isinstance(exc, json.JSONDecodeError):
        return "invalid_json"
    if isinstance(exc, OSError):
        if exc.errno is not None:
            return f"oserror_{exc.errno}"
        return "oserror"
    return exc.__class__.__name__.lower()


def encode_http_request(method: str, path: str, payload: Dict[str, str]) -> bytes:
    body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
    request_lines = [
        f"{method} {path} HTTP/1.1",
        "Host: 127.0.0.1",
        "Content-Type: application/json",
        f"Content-Length: {len(body)}",
        "Connection: close",
        "",
        "",
    ]
    return "\r\n".join(request_lines).encode("utf-8") + body


async def async_http_json_request(port: int,
                                  method: str,
                                  path: str,
                                  payload: Dict[str, str],
                                  timeout: float) -> Tuple[int, Dict[str, object]]:
    reader = None
    writer = None
    try:
        reader, writer = await asyncio.wait_for(asyncio.open_connection("127.0.0.1", port), timeout=timeout)
        writer.write(encode_http_request(method, path, payload))
        await asyncio.wait_for(writer.drain(), timeout=timeout)

        raw_headers = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), timeout=timeout)
        header_text = raw_headers.decode("iso-8859-1")
        header_lines = header_text.split("\r\n")
        status_line = header_lines[0]
        parts = status_line.split(" ", 2)
        if len(parts) < 2:
            raise RuntimeError(f"invalid status line: {status_line}")
        status = int(parts[1])

        headers = {}
        for line in header_lines[1:]:
            if not line or ":" not in line:
                continue
            name, value = line.split(":", 1)
            headers[name.strip().lower()] = value.strip()

        content_length = int(headers.get("content-length", "0"))
        if content_length > 0:
            body = await asyncio.wait_for(reader.readexactly(content_length), timeout=timeout)
        else:
            body = await asyncio.wait_for(reader.read(), timeout=timeout)

        payload_text = body.decode("utf-8") if body else "{}"
        return status, json.loads(payload_text)
    finally:
        if writer is not None:
            writer.close()
            try:
                await writer.wait_closed()
            except Exception:
                pass


async def run_async_burst(port: int,
                          sql_batch: List[str],
                          timeout_seconds: float) -> Tuple[List[Tuple[int, Dict[str, object], float, str]], float, Counter]:
    start_event = asyncio.Event()
    error_counter: Counter = Counter()

    async def run_one(sql: str) -> Tuple[int, Dict[str, object], float, str]:
        await start_event.wait()
        started = time.perf_counter()
        try:
            status, payload = await async_http_json_request(
                port,
                "POST",
                "/query",
                {"sql": sql},
                timeout=timeout_seconds,
            )
            error_label = ""
        except Exception as exc:
            status = 599
            payload = {"error": str(exc)}
            error_label = format_error_label(exc)
            error_counter[error_label] += 1
        elapsed_ms = (time.perf_counter() - started) * 1000.0
        return status, payload, elapsed_ms, error_label

    tasks = [asyncio.create_task(run_one(sql)) for sql in sql_batch]
    await asyncio.sleep(0)

    wall_started = time.perf_counter()
    start_event.set()
    results = await asyncio.gather(*tasks)
    wall_elapsed_ms = (time.perf_counter() - wall_started) * 1000.0
    return results, wall_elapsed_ms, error_counter


def run_single_async_benchmark(server_bin: Path,
                               base_dataset: Path,
                               dataset_rows: int,
                               workers: int,
                               queue_size: int,
                               workload: str,
                               total_requests: int,
                               repeat_index: int,
                               timeout_seconds: float,
                               startup_retries: int,
                               startup_retry_delay_seconds: float,
                               cooldown_seconds: float) -> AsyncSample:
    db_dir = Path(tempfile.mkdtemp(prefix="mini_db_async_dataset_run_", dir="/tmp"))
    shutil.copytree(base_dataset, db_dir, dirs_exist_ok=True)
    server = None
    try:
        last_exc = None
        for attempt in range(1, startup_retries + 1):
            server = ServerProcess(server_bin, db_dir, workers=workers, queue_size=queue_size)
            try:
                server.start()
                break
            except Exception as exc:
                last_exc = exc
                server.stop()
                server = None
                if attempt == startup_retries:
                    raise RuntimeError(
                        f"server failed to become healthy after {startup_retries} attempts"
                    ) from last_exc
                time.sleep(startup_retry_delay_seconds)

        warm_up_runtime(server.port, dataset_rows)
        sql_batch = build_sql_batch(workload, dataset_rows, total_requests)
        results, wall_elapsed_ms, error_counter = asyncio.run(
            run_async_burst(server.port, sql_batch, timeout_seconds)
        )

        latencies = [elapsed_ms for _, _, elapsed_ms, _ in results]
        success_requests = sum(1 for status, _, _, _ in results if status == 200)
        rejected_requests = sum(1 for status, _, _, _ in results if status == 503)
        error_requests = total_requests - success_requests - rejected_requests
        sorted_latencies = sorted(latencies)
        p95_ms = sorted_latencies[max(0, len(sorted_latencies) * 95 // 100 - 1)]

        return AsyncSample(
            row=BenchmarkRow(
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
                avg_ms=sum(latencies) / len(latencies),
                p95_ms=p95_ms,
                max_ms=max(latencies),
            ),
            error_breakdown=error_counter,
        )
    finally:
        if server is not None:
            server.stop()
        if cooldown_seconds > 0:
            time.sleep(cooldown_seconds)
        shutil.rmtree(db_dir, ignore_errors=True)


def write_raw_csv(samples: List[AsyncSample], csv_path: Path) -> None:
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
            "error_breakdown_json",
        ])
        for sample in samples:
            row = sample.row
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
                json.dumps(sample.error_breakdown, ensure_ascii=False, sort_keys=True),
            ])


def build_error_summary(samples: List[AsyncSample]) -> Dict[Tuple[int, str, int, int, int], Counter]:
    grouped: Dict[Tuple[int, str, int, int, int], Counter] = defaultdict(Counter)
    for sample in samples:
        row = sample.row
        key = (row.dataset_rows, row.workload, row.total_requests, row.workers, row.queue_size)
        grouped[key].update(sample.error_breakdown)
    return grouped


def format_top_errors(counter: Counter) -> str:
    if not counter:
        return "없음"
    top_items = counter.most_common(3)
    return ", ".join(f"{label}={count}" for label, count in top_items)


def write_report(summary_rows: List[BenchmarkSummaryRow],
                 samples: List[AsyncSample],
                 summary_csv_path: Path,
                 raw_csv_path: Path,
                 report_path: Path,
                 args: argparse.Namespace) -> None:
    best_rows = choose_best_rows(summary_rows)
    error_summary = build_error_summary(samples)

    lines: List[str] = []
    lines.append("# mini_db_server async 고동시성 스트레스 리포트")
    lines.append("")
    append_reader_guide(lines, "async")
    lines.append("## 측정 조건")
    lines.append("")
    lines.append(f"- 데이터셋 크기: `{', '.join(str(size) for size in args.dataset_sizes)}`행")
    lines.append(f"- 워커 수 후보: `{', '.join(str(worker) for worker in args.workers)}`")
    lines.append(f"- 큐 크기 후보: `{', '.join(str(queue) for queue in args.queues)}`")
    lines.append(f"- 부하 종류: `{', '.join(format_workload_label(workload) for workload in args.workloads)}`")
    lines.append(f"- 동시성 수준: `{', '.join(str(level) for level in args.concurrency_levels)}`")
    lines.append(f"- 반복 횟수: 각 조합당 `{args.repeats}`회")
    lines.append(f"- 요청 타임아웃: `{args.timeout_seconds:.1f}`초")
    lines.append("- 각 run은 `총 요청 수 = 동시 클라이언트 수`로 맞춰 순간 버스트를 만들었습니다.")
    lines.append(f"- 요약 CSV 결과 파일: `{summary_csv_path}`")
    lines.append(f"- raw 샘플 CSV 결과 파일: `{raw_csv_path}`")
    lines.append("")
    lines.append("## 버스트 수준별 최적 조합")
    lines.append("")
    lines.append("| 데이터셋 행 수 | 부하 종류 | 동시 클라이언트 수 | 최적 워커/큐 | 성공 평균+-표준편차 | 503 평균+-표준편차 | 오류 평균+-표준편차 | 평균 ms 평균+-표준편차 | p95 ms 평균+-표준편차 |")
    lines.append("| --- | --- | --- | --- | --- | --- | --- | --- | --- |")
    for key in sorted(best_rows):
        row = best_rows[key]
        lines.append(
            f"| {row.dataset_rows} | {format_workload_label(row.workload)} | {row.total_requests} | {row.workers}/{row.queue_size} | "
            f"{format_mean_stddev(row.success_mean, row.success_stddev, 1)} / {row.total_requests} | "
            f"{format_mean_stddev(row.rejected_mean, row.rejected_stddev, 1)} | "
            f"{format_mean_stddev(row.error_mean, row.error_stddev, 1)} | "
            f"{format_mean_stddev(row.avg_ms_mean, row.avg_ms_stddev)} | "
            f"{format_mean_stddev(row.p95_ms_mean, row.p95_ms_stddev)} |"
        )

    lines.append("")
    lines.append("## 오류 유형 요약")
    lines.append("")
    lines.append("| 데이터셋 행 수 | 부하 종류 | 동시 클라이언트 수 | 워커/큐 | 상위 오류 유형 |")
    lines.append("| --- | --- | --- | --- | --- |")
    for key in sorted(error_summary):
        dataset_rows, workload, concurrency, workers, queue_size = key
        lines.append(
            f"| {dataset_rows} | {format_workload_label(workload)} | {concurrency} | {workers}/{queue_size} | "
            f"{format_top_errors(error_summary[key])} |"
        )

    lines.append("")
    lines.append("## 전체 결과")
    lines.append("")
    for dataset_rows in args.dataset_sizes:
        lines.append(f"### 데이터셋 {dataset_rows}행")
        lines.append("")
        for workload in args.workloads:
            subset = [
                row for row in summary_rows
                if row.dataset_rows == dataset_rows and row.workload == workload
            ]
            subset.sort(key=lambda row: (row.total_requests, row.workers, row.queue_size))
            lines.append(f"#### {format_workload_label(workload)}")
            lines.append("")
            lines.append("| 동시 클라이언트 수 | 워커 수 | 큐 크기 | 반복 수 | 성공 평균+-표준편차 | 503 평균+-표준편차 | 오류 평균+-표준편차 | 평균 ms 평균+-표준편차 | p95 ms 평균+-표준편차 | 최대 ms 평균+-표준편차 | 상위 오류 유형 |")
            lines.append("| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |")
            for row in subset:
                error_key = (row.dataset_rows, row.workload, row.total_requests, row.workers, row.queue_size)
                lines.append(
                    f"| {row.total_requests} | {row.workers} | {row.queue_size} | {row.repeats} | "
                    f"{format_mean_stddev(row.success_mean, row.success_stddev, 1)} | "
                    f"{format_mean_stddev(row.rejected_mean, row.rejected_stddev, 1)} | "
                    f"{format_mean_stddev(row.error_mean, row.error_stddev, 1)} | "
                    f"{format_mean_stddev(row.avg_ms_mean, row.avg_ms_stddev)} | "
                    f"{format_mean_stddev(row.p95_ms_mean, row.p95_ms_stddev)} | "
                    f"{format_mean_stddev(row.max_ms_mean, row.max_ms_stddev)} | "
                    f"{format_top_errors(error_summary.get(error_key, Counter()))} |"
                )
            lines.append("")

    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> None:
    args = parse_args()
    root_dir = Path(args.root_dir).resolve()
    server_bin = (root_dir / args.server_bin).resolve() if not Path(args.server_bin).is_absolute() else Path(args.server_bin)
    summary_csv_path = (root_dir / args.csv_out).resolve()
    raw_csv_path = (root_dir / args.raw_csv_out).resolve()
    report_path = (root_dir / args.report_out).resolve()

    ensure_server_built(server_bin, root_dir)

    datasets_root = Path(tempfile.mkdtemp(prefix="mini_db_async_stress_datasets_", dir="/tmp"))
    samples: List[AsyncSample] = []
    try:
        dataset_dirs = {size: generate_dataset(datasets_root, size) for size in args.dataset_sizes}
        for dataset_rows in args.dataset_sizes:
            for workload in args.workloads:
                for concurrency in args.concurrency_levels:
                    for workers in args.workers:
                        for queue_size in args.queues:
                            for repeat_index in range(1, args.repeats + 1):
                                sample = run_single_async_benchmark(
                                    server_bin=server_bin,
                                    base_dataset=dataset_dirs[dataset_rows],
                                    dataset_rows=dataset_rows,
                                    workers=workers,
                                    queue_size=queue_size,
                                    workload=workload,
                                    total_requests=concurrency,
                                    repeat_index=repeat_index,
                                    timeout_seconds=args.timeout_seconds,
                                    startup_retries=args.startup_retries,
                                    startup_retry_delay_seconds=args.startup_retry_delay_seconds,
                                    cooldown_seconds=args.cooldown_seconds,
                                )
                                samples.append(sample)
                                row = sample.row
                                print(
                                    f"[async] rows={row.dataset_rows} workload={row.workload} cc={row.total_requests} "
                                    f"workers={row.workers} queue={row.queue_size} repeat={row.repeat_index}/{args.repeats} "
                                    f"success={row.success_requests}/{row.total_requests} 503={row.rejected_requests} "
                                    f"errors={row.error_requests} avg_ms={row.avg_ms:.3f} p95_ms={row.p95_ms:.3f} "
                                    f"top_errors={format_top_errors(sample.error_breakdown)}",
                                    flush=True,
                                )

        summary_rows = aggregate_rows([sample.row for sample in samples])
        write_csv(summary_rows, summary_csv_path)
        write_raw_csv(samples, raw_csv_path)
        write_report(summary_rows, samples, summary_csv_path, raw_csv_path, report_path, args)
        print(f"[done] wrote summary CSV to {summary_csv_path}")
        print(f"[done] wrote raw CSV to {raw_csv_path}")
        print(f"[done] wrote report to {report_path}")
    finally:
        shutil.rmtree(datasets_root, ignore_errors=True)


if __name__ == "__main__":
    main()
