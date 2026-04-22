#!/usr/bin/env python3

import argparse
import asyncio
import concurrent.futures
import csv
import re
import shutil
import signal
import statistics
import subprocess
import tempfile
import time
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

from run_api_server_async_stress import run_async_burst
from run_api_server_benchmarks import (
    build_sql_batch,
    copy_dataset,
    generate_dataset,
    http_json_request,
    pick_free_port,
    warm_up_runtime,
)


DEFAULT_REPEATS = 10
DEFAULT_PHASES = ["quick", "expanded", "async"]
DEFAULT_SERVER_MODELS = {
    "serial": "./mini_db_server_serial",
    "thread_pool": "./mini_db_server",
    "thread_per_request": "./mini_db_server_thread_per_req",
}


@dataclass(frozen=True)
class Scenario:
    scenario_id: str
    phase: str
    description: str
    dataset_rows: int
    workload: str
    concurrency: int
    total_requests: int
    workers: int
    queue_size: int
    driver: str


@dataclass
class ResourceStats:
    real_seconds: float
    user_seconds: float
    sys_seconds: float
    max_rss_kb: int
    voluntary_ctx: int
    involuntary_ctx: int


@dataclass
class ComparisonSample:
    repeat_index: int
    model: str
    scenario_id: str
    phase: str
    description: str
    dataset_rows: int
    workload: str
    concurrency: int
    total_requests: int
    workers: int
    queue_size: int
    driver: str
    success_requests: int
    rejected_requests: int
    error_requests: int
    throughput_rps: float
    avg_ms: float
    p95_ms: float
    p99_ms: float
    max_ms: float
    real_seconds: float
    user_seconds: float
    sys_seconds: float
    cpu_util_percent: float
    max_rss_kb: int
    voluntary_ctx: int
    involuntary_ctx: int


@dataclass
class ComparisonSummary:
    model: str
    scenario_id: str
    phase: str
    description: str
    dataset_rows: int
    workload: str
    concurrency: int
    total_requests: int
    workers: int
    queue_size: int
    driver: str
    repeats: int
    success_mean: float
    success_stddev: float
    rejected_mean: float
    rejected_stddev: float
    error_mean: float
    error_stddev: float
    throughput_rps_mean: float
    throughput_rps_stddev: float
    avg_ms_mean: float
    avg_ms_stddev: float
    p95_ms_mean: float
    p95_ms_stddev: float
    p99_ms_mean: float
    p99_ms_stddev: float
    max_ms_mean: float
    max_ms_stddev: float
    cpu_util_percent_mean: float
    cpu_util_percent_stddev: float
    max_rss_kb_mean: float
    max_rss_kb_stddev: float
    voluntary_ctx_mean: float
    involuntary_ctx_mean: float


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Compare serial, thread-pool, and thread-per-request server models."
    )
    parser.add_argument("--root-dir", default=".")
    parser.add_argument("--repeats", type=int, default=DEFAULT_REPEATS)
    parser.add_argument("--phases", nargs="+", default=DEFAULT_PHASES)
    parser.add_argument("--serial-bin", default=DEFAULT_SERVER_MODELS["serial"])
    parser.add_argument("--thread-pool-bin", default=DEFAULT_SERVER_MODELS["thread_pool"])
    parser.add_argument("--thread-per-request-bin", default=DEFAULT_SERVER_MODELS["thread_per_request"])
    parser.add_argument("--summary-csv-out", default="reports/api_server_model_comparison_results.csv")
    parser.add_argument("--raw-csv-out", default="reports/api_server_model_comparison_samples.csv")
    parser.add_argument("--report-out", default="reports/api_server_model_comparison_report.md")
    parser.add_argument("--timeout-seconds", type=float, default=30.0)
    parser.add_argument("--async-timeout-seconds", type=float, default=60.0)
    parser.add_argument("--scenario-ids", nargs="*", default=[])
    return parser.parse_args()


def build_scenarios(phases: Sequence[str]) -> List[Scenario]:
    scenarios: List[Scenario] = []

    if "quick" in phases:
        for workload in ("select-only", "insert-only", "mixed"):
            for concurrency in (1, 32, 128):
                scenarios.append(
                    Scenario(
                        scenario_id=f"quick_100k_{workload}_c{concurrency}",
                        phase="quick",
                        description=f"100k rows, {workload}, concurrency {concurrency}",
                        dataset_rows=100_000,
                        workload=workload,
                        concurrency=concurrency,
                        total_requests=512,
                        workers=4,
                        queue_size=128,
                        driver="thread",
                    )
                )
            scenarios.append(
                Scenario(
                    scenario_id=f"quick_100k_{workload}_c512",
                    phase="quick",
                    description=f"100k rows, {workload}, burst concurrency 512",
                    dataset_rows=100_000,
                    workload=workload,
                    concurrency=512,
                    total_requests=512,
                    workers=8,
                    queue_size=512,
                    driver="thread",
                )
            )

    if "expanded" in phases:
        scenarios.extend(
            [
                Scenario(
                    scenario_id="expanded_10k_mixed_c128",
                    phase="expanded",
                    description="10k rows, mixed, steady concurrency 128",
                    dataset_rows=10_000,
                    workload="mixed",
                    concurrency=128,
                    total_requests=512,
                    workers=4,
                    queue_size=128,
                    driver="thread",
                ),
                Scenario(
                    scenario_id="expanded_1m_mixed_c128",
                    phase="expanded",
                    description="1M rows, mixed, steady concurrency 128",
                    dataset_rows=1_000_000,
                    workload="mixed",
                    concurrency=128,
                    total_requests=512,
                    workers=8,
                    queue_size=128,
                    driver="thread",
                ),
                Scenario(
                    scenario_id="expanded_100k_mixed_c1024",
                    phase="expanded",
                    description="100k rows, mixed, burst concurrency 1024",
                    dataset_rows=100_000,
                    workload="mixed",
                    concurrency=1024,
                    total_requests=1024,
                    workers=8,
                    queue_size=512,
                    driver="thread",
                ),
                Scenario(
                    scenario_id="expanded_1m_mixed_c1024",
                    phase="expanded",
                    description="1M rows, mixed, burst concurrency 1024",
                    dataset_rows=1_000_000,
                    workload="mixed",
                    concurrency=1024,
                    total_requests=1024,
                    workers=8,
                    queue_size=512,
                    driver="thread",
                ),
            ]
        )

    if "async" in phases:
        for dataset_rows in (100_000, 1_000_000):
            for concurrency in (2_000, 5_000):
                scenarios.append(
                    Scenario(
                        scenario_id=f"async_{dataset_rows}_mixed_c{concurrency}",
                        phase="async",
                        description=f"{dataset_rows:,} rows, mixed, async burst {concurrency}",
                        dataset_rows=dataset_rows,
                        workload="mixed",
                        concurrency=concurrency,
                        total_requests=concurrency,
                        workers=8,
                        queue_size=512,
                        driver="async",
                    )
                )

    return scenarios


def compute_percentile(latencies: Sequence[float], percentile: float) -> float:
    if not latencies:
        return 0.0
    ordered = sorted(latencies)
    index = max(0, int((len(ordered) * percentile + 99.9999) // 100) - 1)
    return ordered[min(index, len(ordered) - 1)]


def mean_stddev(values: Iterable[float]) -> Tuple[float, float]:
    materialized = list(values)
    if not materialized:
        return 0.0, 0.0
    mean_value = statistics.fmean(materialized)
    stddev_value = statistics.stdev(materialized) if len(materialized) > 1 else 0.0
    return mean_value, stddev_value


def parse_resource_stats(log_text: str) -> ResourceStats:
    timing_matches = re.findall(r"^\s*([0-9.]+)\s+real\s+([0-9.]+)\s+user\s+([0-9.]+)\s+sys\s*$",
                                log_text, re.MULTILINE)
    rss_matches = re.findall(r"^\s*(\d+)\s+maximum resident set size$", log_text, re.MULTILINE)
    voluntary_matches = re.findall(r"^\s*(\d+)\s+voluntary context switches$", log_text, re.MULTILINE)
    involuntary_matches = re.findall(r"^\s*(\d+)\s+involuntary context switches$", log_text, re.MULTILINE)

    if timing_matches:
        real_seconds, user_seconds, sys_seconds = map(float, timing_matches[-1])
    else:
        real_seconds, user_seconds, sys_seconds = 0.0, 0.0, 0.0

    return ResourceStats(
        real_seconds=real_seconds,
        user_seconds=user_seconds,
        sys_seconds=sys_seconds,
        max_rss_kb=int(rss_matches[-1]) if rss_matches else 0,
        voluntary_ctx=int(voluntary_matches[-1]) if voluntary_matches else 0,
        involuntary_ctx=int(involuntary_matches[-1]) if involuntary_matches else 0,
    )


class TimedServerProcess:
    def __init__(self, server_bin: Path, db_dir: Path, workers: int, queue_size: int):
        self.server_bin = server_bin
        self.db_dir = db_dir
        self.workers = workers
        self.queue_size = queue_size
        self.port = pick_free_port()
        self.log_file = tempfile.NamedTemporaryFile(prefix="server_model_compare_", suffix=".log", delete=False)
        self.process: Optional[subprocess.Popen] = None

    def start(self) -> None:
        self.process = subprocess.Popen(
            [
                "/usr/bin/time",
                "-l",
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
        deadline = time.time() + 20.0
        while time.time() < deadline:
            if self.process is not None and self.process.poll() is not None:
                raise RuntimeError(f"server exited during startup:\n{self.read_log_tail()}")
            try:
                status, payload = http_json_request(self.port, "GET", "/health")
                if status == 200 and payload.get("success") is True:
                    return
            except Exception:
                pass
            time.sleep(0.1)
        raise RuntimeError(f"server did not become healthy:\n{self.read_log_tail()}")

    def stop(self) -> ResourceStats:
        if self.process is None:
            return ResourceStats(0.0, 0.0, 0.0, 0, 0, 0)
        self.process.send_signal(signal.SIGINT)
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=5)
        self.log_file.flush()
        self.log_file.close()
        log_text = Path(self.log_file.name).read_text(encoding="utf-8", errors="replace")
        self.process = None
        return parse_resource_stats(log_text)

    def read_log_tail(self) -> str:
        self.log_file.flush()
        return Path(self.log_file.name).read_text(encoding="utf-8", errors="replace")[-4000:]


def ensure_server_binaries(root_dir: Path, server_bins: Dict[str, Path]) -> None:
    targets = sorted({path.name for path in server_bins.values()})
    subprocess.run(["make", *targets], cwd=root_dir, check=True)


def run_thread_sample(server: TimedServerProcess,
                      scenario: Scenario,
                      timeout_seconds: float) -> Tuple[List[float], int, int, int, float]:
    sql_batch = build_sql_batch(scenario.workload, scenario.dataset_rows, scenario.total_requests)

    def run_one(sql: str) -> Tuple[int, float]:
        started = time.perf_counter()
        try:
            status, _ = http_json_request(server.port, "POST", "/query", {"sql": sql}, timeout=timeout_seconds)
        except Exception:
            status = 599
        elapsed_ms = (time.perf_counter() - started) * 1000.0
        return status, elapsed_ms

    wall_started = time.perf_counter()
    with concurrent.futures.ThreadPoolExecutor(max_workers=scenario.concurrency) as executor:
        results = list(executor.map(run_one, sql_batch))
    wall_elapsed_ms = (time.perf_counter() - wall_started) * 1000.0

    latencies = [elapsed_ms for _, elapsed_ms in results]
    success_requests = sum(1 for status, _ in results if status == 200)
    rejected_requests = sum(1 for status, _ in results if status == 503)
    error_requests = scenario.total_requests - success_requests - rejected_requests
    return latencies, success_requests, rejected_requests, error_requests, wall_elapsed_ms


def run_async_sample(server: TimedServerProcess,
                     scenario: Scenario,
                     timeout_seconds: float) -> Tuple[List[float], int, int, int, float]:
    sql_batch = build_sql_batch(scenario.workload, scenario.dataset_rows, scenario.total_requests)
    results, wall_elapsed_ms, _ = asyncio.run(run_async_burst(server.port, sql_batch, timeout_seconds))
    latencies = [elapsed_ms for _, _, elapsed_ms, _ in results]
    success_requests = sum(1 for status, _, _, _ in results if status == 200)
    rejected_requests = sum(1 for status, _, _, _ in results if status == 503)
    error_requests = scenario.total_requests - success_requests - rejected_requests
    return latencies, success_requests, rejected_requests, error_requests, wall_elapsed_ms


def run_single_sample(model: str,
                      server_bin: Path,
                      scenario: Scenario,
                      base_dataset: Path,
                      repeat_index: int,
                      timeout_seconds: float,
                      async_timeout_seconds: float) -> ComparisonSample:
    db_dir = copy_dataset(base_dataset)
    server = TimedServerProcess(server_bin, db_dir, scenario.workers, scenario.queue_size)
    try:
        server.start()
        warm_up_runtime(server.port, scenario.dataset_rows)

        if scenario.driver == "thread":
            latencies, success_requests, rejected_requests, error_requests, wall_elapsed_ms = run_thread_sample(
                server, scenario, timeout_seconds
            )
        else:
            latencies, success_requests, rejected_requests, error_requests, wall_elapsed_ms = run_async_sample(
                server, scenario, async_timeout_seconds
            )
    finally:
        resource_stats = server.stop()
        shutil.rmtree(db_dir, ignore_errors=True)

    throughput_rps = 0.0
    if wall_elapsed_ms > 0.0:
        throughput_rps = success_requests / (wall_elapsed_ms / 1000.0)
    cpu_util_percent = 0.0
    if resource_stats.real_seconds > 0.0:
        cpu_util_percent = ((resource_stats.user_seconds + resource_stats.sys_seconds) /
                            resource_stats.real_seconds) * 100.0

    return ComparisonSample(
        repeat_index=repeat_index,
        model=model,
        scenario_id=scenario.scenario_id,
        phase=scenario.phase,
        description=scenario.description,
        dataset_rows=scenario.dataset_rows,
        workload=scenario.workload,
        concurrency=scenario.concurrency,
        total_requests=scenario.total_requests,
        workers=scenario.workers,
        queue_size=scenario.queue_size,
        driver=scenario.driver,
        success_requests=success_requests,
        rejected_requests=rejected_requests,
        error_requests=error_requests,
        throughput_rps=throughput_rps,
        avg_ms=statistics.fmean(latencies) if latencies else 0.0,
        p95_ms=compute_percentile(latencies, 95.0),
        p99_ms=compute_percentile(latencies, 99.0),
        max_ms=max(latencies) if latencies else 0.0,
        real_seconds=resource_stats.real_seconds,
        user_seconds=resource_stats.user_seconds,
        sys_seconds=resource_stats.sys_seconds,
        cpu_util_percent=cpu_util_percent,
        max_rss_kb=resource_stats.max_rss_kb,
        voluntary_ctx=resource_stats.voluntary_ctx,
        involuntary_ctx=resource_stats.involuntary_ctx,
    )


def aggregate_samples(samples: List[ComparisonSample]) -> List[ComparisonSummary]:
    grouped: Dict[Tuple[str, str], List[ComparisonSample]] = defaultdict(list)
    for sample in samples:
        grouped[(sample.model, sample.scenario_id)].append(sample)

    summaries: List[ComparisonSummary] = []
    for key in sorted(grouped):
        runs = grouped[key]
        reference = runs[0]
        success_mean, success_stddev = mean_stddev(run.success_requests for run in runs)
        rejected_mean, rejected_stddev = mean_stddev(run.rejected_requests for run in runs)
        error_mean, error_stddev = mean_stddev(run.error_requests for run in runs)
        throughput_rps_mean, throughput_rps_stddev = mean_stddev(run.throughput_rps for run in runs)
        avg_ms_mean, avg_ms_stddev = mean_stddev(run.avg_ms for run in runs)
        p95_ms_mean, p95_ms_stddev = mean_stddev(run.p95_ms for run in runs)
        p99_ms_mean, p99_ms_stddev = mean_stddev(run.p99_ms for run in runs)
        max_ms_mean, max_ms_stddev = mean_stddev(run.max_ms for run in runs)
        cpu_util_percent_mean, cpu_util_percent_stddev = mean_stddev(run.cpu_util_percent for run in runs)
        max_rss_kb_mean, max_rss_kb_stddev = mean_stddev(run.max_rss_kb for run in runs)
        voluntary_ctx_mean, _ = mean_stddev(run.voluntary_ctx for run in runs)
        involuntary_ctx_mean, _ = mean_stddev(run.involuntary_ctx for run in runs)
        summaries.append(
            ComparisonSummary(
                model=reference.model,
                scenario_id=reference.scenario_id,
                phase=reference.phase,
                description=reference.description,
                dataset_rows=reference.dataset_rows,
                workload=reference.workload,
                concurrency=reference.concurrency,
                total_requests=reference.total_requests,
                workers=reference.workers,
                queue_size=reference.queue_size,
                driver=reference.driver,
                repeats=len(runs),
                success_mean=success_mean,
                success_stddev=success_stddev,
                rejected_mean=rejected_mean,
                rejected_stddev=rejected_stddev,
                error_mean=error_mean,
                error_stddev=error_stddev,
                throughput_rps_mean=throughput_rps_mean,
                throughput_rps_stddev=throughput_rps_stddev,
                avg_ms_mean=avg_ms_mean,
                avg_ms_stddev=avg_ms_stddev,
                p95_ms_mean=p95_ms_mean,
                p95_ms_stddev=p95_ms_stddev,
                p99_ms_mean=p99_ms_mean,
                p99_ms_stddev=p99_ms_stddev,
                max_ms_mean=max_ms_mean,
                max_ms_stddev=max_ms_stddev,
                cpu_util_percent_mean=cpu_util_percent_mean,
                cpu_util_percent_stddev=cpu_util_percent_stddev,
                max_rss_kb_mean=max_rss_kb_mean,
                max_rss_kb_stddev=max_rss_kb_stddev,
                voluntary_ctx_mean=voluntary_ctx_mean,
                involuntary_ctx_mean=involuntary_ctx_mean,
            )
        )
    return summaries


def write_raw_csv(samples: List[ComparisonSample], csv_path: Path) -> None:
    csv_path.parent.mkdir(parents=True, exist_ok=True)
    with csv_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow([
            "repeat_index",
            "model",
            "scenario_id",
            "phase",
            "description",
            "dataset_rows",
            "workload",
            "concurrency",
            "total_requests",
            "workers",
            "queue_size",
            "driver",
            "success_requests",
            "rejected_requests",
            "error_requests",
            "throughput_rps",
            "avg_ms",
            "p95_ms",
            "p99_ms",
            "max_ms",
            "real_seconds",
            "user_seconds",
            "sys_seconds",
            "cpu_util_percent",
            "max_rss_kb",
            "voluntary_ctx",
            "involuntary_ctx",
        ])
        for sample in samples:
            writer.writerow([
                sample.repeat_index,
                sample.model,
                sample.scenario_id,
                sample.phase,
                sample.description,
                sample.dataset_rows,
                sample.workload,
                sample.concurrency,
                sample.total_requests,
                sample.workers,
                sample.queue_size,
                sample.driver,
                sample.success_requests,
                sample.rejected_requests,
                sample.error_requests,
                f"{sample.throughput_rps:.3f}",
                f"{sample.avg_ms:.3f}",
                f"{sample.p95_ms:.3f}",
                f"{sample.p99_ms:.3f}",
                f"{sample.max_ms:.3f}",
                f"{sample.real_seconds:.3f}",
                f"{sample.user_seconds:.3f}",
                f"{sample.sys_seconds:.3f}",
                f"{sample.cpu_util_percent:.3f}",
                f"{sample.max_rss_kb:.1f}",
                f"{sample.voluntary_ctx:.1f}",
                f"{sample.involuntary_ctx:.1f}",
            ])


def write_summary_csv(rows: List[ComparisonSummary], csv_path: Path) -> None:
    csv_path.parent.mkdir(parents=True, exist_ok=True)
    with csv_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow([
            "model",
            "scenario_id",
            "phase",
            "description",
            "dataset_rows",
            "workload",
            "concurrency",
            "total_requests",
            "workers",
            "queue_size",
            "driver",
            "repeats",
            "success_mean",
            "success_stddev",
            "rejected_mean",
            "rejected_stddev",
            "error_mean",
            "error_stddev",
            "throughput_rps_mean",
            "throughput_rps_stddev",
            "avg_ms_mean",
            "avg_ms_stddev",
            "p95_ms_mean",
            "p95_ms_stddev",
            "p99_ms_mean",
            "p99_ms_stddev",
            "max_ms_mean",
            "max_ms_stddev",
            "cpu_util_percent_mean",
            "cpu_util_percent_stddev",
            "max_rss_kb_mean",
            "max_rss_kb_stddev",
            "voluntary_ctx_mean",
            "involuntary_ctx_mean",
        ])
        for row in rows:
            writer.writerow([
                row.model,
                row.scenario_id,
                row.phase,
                row.description,
                row.dataset_rows,
                row.workload,
                row.concurrency,
                row.total_requests,
                row.workers,
                row.queue_size,
                row.driver,
                row.repeats,
                f"{row.success_mean:.3f}",
                f"{row.success_stddev:.3f}",
                f"{row.rejected_mean:.3f}",
                f"{row.rejected_stddev:.3f}",
                f"{row.error_mean:.3f}",
                f"{row.error_stddev:.3f}",
                f"{row.throughput_rps_mean:.3f}",
                f"{row.throughput_rps_stddev:.3f}",
                f"{row.avg_ms_mean:.3f}",
                f"{row.avg_ms_stddev:.3f}",
                f"{row.p95_ms_mean:.3f}",
                f"{row.p95_ms_stddev:.3f}",
                f"{row.p99_ms_mean:.3f}",
                f"{row.p99_ms_stddev:.3f}",
                f"{row.max_ms_mean:.3f}",
                f"{row.max_ms_stddev:.3f}",
                f"{row.cpu_util_percent_mean:.3f}",
                f"{row.cpu_util_percent_stddev:.3f}",
                f"{row.max_rss_kb_mean:.3f}",
                f"{row.max_rss_kb_stddev:.3f}",
                f"{row.voluntary_ctx_mean:.3f}",
                f"{row.involuntary_ctx_mean:.3f}",
            ])


def row_by_model(rows: Iterable[ComparisonSummary], scenario_id: str) -> Dict[str, ComparisonSummary]:
    mapping: Dict[str, ComparisonSummary] = {}
    for row in rows:
        if row.scenario_id == scenario_id:
            mapping[row.model] = row
    return mapping


def find_best_model(rows: Iterable[ComparisonSummary], scenario_id: str) -> Optional[ComparisonSummary]:
    candidates = list(row_by_model(rows, scenario_id).values())
    if not candidates:
        return None

    def score(row: ComparisonSummary) -> Tuple[float, float, float, float, float, float]:
        total_requests = max(1.0, row.total_requests)
        unexpected_error_ratio = row.error_mean / total_requests
        total_context_switch = row.voluntary_ctx_mean + row.involuntary_ctx_mean
        return (
            unexpected_error_ratio,
            row.p95_ms_mean,
            row.max_rss_kb_mean,
            total_context_switch,
            row.rejected_mean,
            -row.throughput_rps_mean,
        )

    return min(candidates, key=score)


def find_fastest_model(rows: Iterable[ComparisonSummary], scenario_id: str) -> Optional[ComparisonSummary]:
    candidates = list(row_by_model(rows, scenario_id).values())
    if not candidates:
        return None
    return max(candidates, key=lambda row: row.throughput_rps_mean)


def format_mean_stddev(mean_value: float, stddev_value: float, digits: int = 1) -> str:
    return f"{mean_value:.{digits}f} +- {stddev_value:.{digits}f}"


def format_rss_mb(mean_value: float, stddev_value: float) -> str:
    return f"{mean_value / (1024.0 * 1024.0):.1f} +- {stddev_value / (1024.0 * 1024.0):.1f}"


def model_label(model: str) -> str:
    if model == "serial":
        return "직렬 서버"
    if model == "thread_pool":
        return "스레드풀 서버"
    return "요청당 스레드 서버"


def workload_label(workload: str) -> str:
    if workload == "select-only":
        return "SELECT 전용"
    if workload == "insert-only":
        return "INSERT 전용"
    return "혼합"


def phase_label(phase: str) -> str:
    if phase == "quick":
        return "기본 비교"
    if phase == "expanded":
        return "확장 비교"
    return "고동시성 async"


def build_mermaid_throughput(rows: List[ComparisonSummary]) -> str:
    scenario_ids = [
        "quick_100k_mixed_c1",
        "quick_100k_mixed_c32",
        "quick_100k_mixed_c128",
        "quick_100k_mixed_c512",
    ]
    labels = ["1", "32", "128", "512"]
    series = {
        "직렬": [],
        "스레드풀": [],
        "요청당 스레드": [],
    }
    for scenario_id in scenario_ids:
        by_model = row_by_model(rows, scenario_id)
        if "serial" not in by_model or "thread_pool" not in by_model or "thread_per_request" not in by_model:
            return "_기본 비교 시나리오가 없어 처리량 그래프를 생략했습니다._"
        series["직렬"].append(round(by_model["serial"].throughput_rps_mean, 1))
        series["스레드풀"].append(round(by_model["thread_pool"].throughput_rps_mean, 1))
        series["요청당 스레드"].append(round(by_model["thread_per_request"].throughput_rps_mean, 1))
    y_max = max(
        max(series["직렬"], default=0.0),
        max(series["스레드풀"], default=0.0),
        max(series["요청당 스레드"], default=0.0),
        1.0,
    )
    y_axis_max = int(((y_max * 1.2) // 100) + 1) * 100

    return "\n".join([
        "```mermaid",
        "xychart-beta",
        '    title "100k 혼합 workload 처리량 비교"',
        '    x-axis "동시 요청 수" ["1", "32", "128", "512"]',
        f'    y-axis "성공 처리량 (req/s)" 0 --> {y_axis_max}',
        f'    line "직렬" [{", ".join(str(value) for value in series["직렬"])}]',
        f'    line "스레드풀" [{", ".join(str(value) for value in series["스레드풀"])}]',
        f'    line "요청당 스레드" [{", ".join(str(value) for value in series["요청당 스레드"])}]',
        "```",
    ])


def build_mermaid_p95(rows: List[ComparisonSummary]) -> str:
    scenario_ids = [
        "quick_100k_select-only_c1",
        "quick_100k_select-only_c32",
        "quick_100k_select-only_c128",
        "quick_100k_select-only_c512",
    ]
    series = {
        "직렬": [],
        "스레드풀": [],
        "요청당 스레드": [],
    }
    for scenario_id in scenario_ids:
        by_model = row_by_model(rows, scenario_id)
        if "serial" not in by_model or "thread_pool" not in by_model or "thread_per_request" not in by_model:
            return "_기본 비교 시나리오가 없어 p95 그래프를 생략했습니다._"
        series["직렬"].append(round(by_model["serial"].p95_ms_mean, 1))
        series["스레드풀"].append(round(by_model["thread_pool"].p95_ms_mean, 1))
        series["요청당 스레드"].append(round(by_model["thread_per_request"].p95_ms_mean, 1))
    y_max = max(
        max(series["직렬"], default=0.0),
        max(series["스레드풀"], default=0.0),
        max(series["요청당 스레드"], default=0.0),
        1.0,
    )
    y_axis_max = int(((y_max * 1.2) // 50) + 1) * 50

    return "\n".join([
        "```mermaid",
        "xychart-beta",
        '    title "100k SELECT 전용 p95 지연 비교"',
        '    x-axis "동시 요청 수" ["1", "32", "128", "512"]',
        f'    y-axis "p95 지연 (ms)" 0 --> {y_axis_max}',
        f'    line "직렬" [{", ".join(str(value) for value in series["직렬"])}]',
        f'    line "스레드풀" [{", ".join(str(value) for value in series["스레드풀"])}]',
        f'    line "요청당 스레드" [{", ".join(str(value) for value in series["요청당 스레드"])}]',
        "```",
    ])


def build_highlights(rows: List[ComparisonSummary], scenarios: List[Scenario]) -> List[str]:
    highlights: List[str] = []

    for scenario_id in (
        "quick_100k_select-only_c128",
        "quick_100k_insert-only_c128",
        "quick_100k_mixed_c128",
        "expanded_100k_mixed_c1024",
        "async_100000_mixed_c5000",
    ):
        best = find_best_model(rows, scenario_id)
        fastest = find_fastest_model(rows, scenario_id)
        by_model = row_by_model(rows, scenario_id)
        if best is None or "serial" not in by_model:
            continue
        serial_row = by_model["serial"]
        speedup = 0.0
        if fastest is not None and serial_row.throughput_rps_mean > 0.0:
            speedup = fastest.throughput_rps_mean / serial_row.throughput_rps_mean
        highlights.append(
            f"`{scenario_id}`에서는 운영 관점 추천 모델이 {model_label(best.model)}였고, "
            f"순수 처리량 최고 모델은 {model_label(fastest.model) if fastest is not None else model_label(best.model)}였습니다. "
            f"최고 처리량 기준 직렬 대비 속도 차이는 약 `{speedup:.2f}배`였습니다."
        )

    return highlights


def build_top_table(rows: List[ComparisonSummary], scenarios: List[Scenario]) -> List[str]:
    lines = [
        "| 시나리오 | 운영 관점 추천 | 처리량 최고 | 이유 |",
        "| --- | --- | --- | --- |",
    ]
    for scenario in scenarios:
        best = find_best_model(rows, scenario.scenario_id)
        fastest = find_fastest_model(rows, scenario.scenario_id)
        if best is None:
            continue
        failure_ratio = (best.rejected_mean + best.error_mean) / max(1.0, best.total_requests)
        lines.append(
            f"| {scenario.description} | {model_label(best.model)} | "
            f"{model_label(fastest.model) if fastest is not None else model_label(best.model)} | "
            f"실패율 `{failure_ratio:.2%}`, 처리량 `{best.throughput_rps_mean:.1f} req/s`, "
            f"p95 `{best.p95_ms_mean:.1f} ms` |"
        )
    return lines


def build_phase_tables(rows: List[ComparisonSummary], phases: Sequence[str]) -> List[str]:
    lines: List[str] = []
    for phase in phases:
        phase_rows = [row for row in rows if row.phase == phase]
        if not phase_rows:
            continue
        lines.append(f"## {phase_label(phase)} 전체 결과")
        lines.append("")
        lines.append("| 모델 | 시나리오 | 처리량(req/s) | 평균 지연(ms) | p95(ms) | 성공 | 503 | 오류 | RSS(MB) | Vol CS | Invol CS |")
        lines.append("| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |")
        for row in phase_rows:
            lines.append(
                f"| {model_label(row.model)} | {row.description} | "
                f"{format_mean_stddev(row.throughput_rps_mean, row.throughput_rps_stddev)} | "
                f"{format_mean_stddev(row.avg_ms_mean, row.avg_ms_stddev)} | "
                f"{format_mean_stddev(row.p95_ms_mean, row.p95_ms_stddev)} | "
                f"{format_mean_stddev(row.success_mean, row.success_stddev, 0)} | "
                f"{format_mean_stddev(row.rejected_mean, row.rejected_stddev, 0)} | "
                f"{format_mean_stddev(row.error_mean, row.error_stddev, 0)} | "
                f"{format_rss_mb(row.max_rss_kb_mean, row.max_rss_kb_stddev)} | "
                f"{row.voluntary_ctx_mean:.0f} | {row.involuntary_ctx_mean:.0f} |"
            )
        lines.append("")
    return lines


def get_row(rows: List[ComparisonSummary], scenario_id: str, model: str) -> ComparisonSummary:
    for row in rows:
        if row.scenario_id == scenario_id and row.model == model:
            return row
    raise KeyError(f"missing row for {scenario_id} / {model}")


def build_key_findings(rows: List[ComparisonSummary]) -> List[str]:
    select_32_serial = get_row(rows, "quick_100k_select-only_c32", "serial")
    select_32_pool = get_row(rows, "quick_100k_select-only_c32", "thread_pool")
    insert_32_serial = get_row(rows, "quick_100k_insert-only_c32", "serial")
    insert_32_pool = get_row(rows, "quick_100k_insert-only_c32", "thread_pool")
    mixed_128_serial = get_row(rows, "quick_100k_mixed_c128", "serial")
    mixed_128_pool = get_row(rows, "quick_100k_mixed_c128", "thread_pool")
    burst_1024_serial = get_row(rows, "expanded_100k_mixed_c1024", "serial")
    burst_1024_pool = get_row(rows, "expanded_100k_mixed_c1024", "thread_pool")
    async_2000_serial = get_row(rows, "async_100000_mixed_c2000", "serial")
    async_2000_pool = get_row(rows, "async_100000_mixed_c2000", "thread_pool")
    async_2000_tpr = get_row(rows, "async_100000_mixed_c2000", "thread_per_request")
    async_5000_pool = get_row(rows, "async_100000_mixed_c5000", "thread_pool")
    async_5000_tpr = get_row(rows, "async_100000_mixed_c5000", "thread_per_request")

    return [
        "기본 비교(100k, 동시성 1~128)에서는 세 모델의 순수 처리량 차이가 생각보다 크지 않았습니다. "
        "읽기 전용 `concurrency 32`에서 스레드풀은 직렬 대비 약 "
        f"`{select_32_pool.throughput_rps_mean / select_32_serial.throughput_rps_mean:.2f}배` 빨랐지만, "
        "그 외 다수 구간은 비슷하거나 직렬이 앞섰습니다.",
        "쓰기 중심(`insert-only`)에서는 병렬 이점이 크지 않았습니다. "
        f"`concurrency 32`에서 직렬은 `{insert_32_serial.throughput_rps_mean:.1f} req/s`, "
        f"스레드풀은 `{insert_32_pool.throughput_rps_mean:.1f} req/s`로, write lock 병목이 더 크게 작용했습니다.",
        "혼합 workload에서는 스레드풀이 절대 처리량보다 `p95 안정화` 쪽에서 더 자주 장점을 보였습니다. "
        f"`100k mixed / concurrency 128`에서 직렬의 처리량은 더 높았지만 "
        f"(`{mixed_128_serial.throughput_rps_mean:.1f} req/s` vs `{mixed_128_pool.throughput_rps_mean:.1f} req/s`), "
        f"p95는 스레드풀이 더 낮았습니다 (`{mixed_128_pool.p95_ms_mean:.1f} ms` vs `{mixed_128_serial.p95_ms_mean:.1f} ms`).",
        "순간 부하가 커지면 스레드풀의 장점이 더 분명해졌습니다. "
        f"`100k mixed / burst 1024`에서 스레드풀은 직렬보다 처리량이 높고 "
        f"(`{burst_1024_pool.throughput_rps_mean:.1f}` vs `{burst_1024_serial.throughput_rps_mean:.1f} req/s`), "
        f"p95도 더 낮았습니다 (`{burst_1024_pool.p95_ms_mean:.1f}` vs `{burst_1024_serial.p95_ms_mean:.1f} ms`).",
        "초고동시성 과부하(`async 2000`, `async 5000`)에서는 해석이 달라집니다. "
        f"직렬은 `503` 없이 전부 받아주지만 p95가 `약 {async_2000_serial.p95_ms_mean / 1000.0:.1f}~{get_row(rows, 'async_100000_mixed_c5000', 'serial').p95_ms_mean / 1000.0:.1f}초`까지 늘어났습니다.",
        "같은 과부하 구간에서 요청당 스레드 방식은 `async 2000`에서는 성공 처리량이 가장 높았지만, "
        f"메모리는 스레드풀보다 약 `{async_2000_tpr.max_rss_kb_mean / async_2000_pool.max_rss_kb_mean:.1f}배`, "
        f"context switch는 약 `{(async_2000_tpr.voluntary_ctx_mean + async_2000_tpr.involuntary_ctx_mean) / (async_2000_pool.voluntary_ctx_mean + async_2000_pool.involuntary_ctx_mean):.1f}배` 많았습니다.",
        "정리하면 이번 환경에서 스레드풀의 가장 큰 장점은 '항상 제일 빠름'이 아니라 "
        "`burst와 overload에서 지연을 통제하면서, 요청당 스레드보다 훨씬 적은 비용으로 버틴다`는 점이었습니다.",
    ]


def build_snapshot_table(rows: List[ComparisonSummary]) -> List[str]:
    scenarios = [
        ("100k SELECT / 동시성 32", "quick_100k_select-only_c32"),
        ("100k INSERT / 동시성 32", "quick_100k_insert-only_c32"),
        ("100k 혼합 / 동시성 128", "quick_100k_mixed_c128"),
        ("100k 혼합 / burst 1024", "expanded_100k_mixed_c1024"),
        ("100k 혼합 / async 2000", "async_100000_mixed_c2000"),
        ("100k 혼합 / async 5000", "async_100000_mixed_c5000"),
    ]
    lines = [
        "| 대표 시나리오 | 직렬 서버 | 스레드풀 서버 | 요청당 스레드 서버 | 읽는 포인트 |",
        "| --- | --- | --- | --- | --- |",
    ]
    for label, scenario_id in scenarios:
        serial = get_row(rows, scenario_id, "serial")
        pool = get_row(rows, scenario_id, "thread_pool")
        tpr = get_row(rows, scenario_id, "thread_per_request")
        if scenario_id.startswith("async_"):
            note = "직렬은 전부 수용하지만 매우 오래 기다리게 하고, 병렬 두 방식은 빠르게 503을 반환합니다."
        elif "insert" in scenario_id:
            note = "write lock 영향이 커서 병렬 이점이 작습니다."
        elif "1024" in scenario_id:
            note = "순간 부하가 커질수록 스레드풀이 균형이 좋습니다."
        else:
            note = "기본 부하에서는 세 모델 차이가 크지 않고, p95와 운영 비용 차이를 함께 봐야 합니다."
        lines.append(
            f"| {label} | "
            f"`{serial.throughput_rps_mean:.0f} req/s`, p95 `{serial.p95_ms_mean:.1f} ms`, RSS `{serial.max_rss_kb_mean / (1024.0 * 1024.0):.1f} MB` | "
            f"`{pool.throughput_rps_mean:.0f} req/s`, p95 `{pool.p95_ms_mean:.1f} ms`, RSS `{pool.max_rss_kb_mean / (1024.0 * 1024.0):.1f} MB` | "
            f"`{tpr.throughput_rps_mean:.0f} req/s`, p95 `{tpr.p95_ms_mean:.1f} ms`, RSS `{tpr.max_rss_kb_mean / (1024.0 * 1024.0):.1f} MB` | "
            f"{note} |"
        )
    return lines


def write_report(report_path: Path,
                 rows: List[ComparisonSummary],
                 scenarios: List[Scenario],
                 args: argparse.Namespace) -> None:
    report_path.parent.mkdir(parents=True, exist_ok=True)
    lines: List[str] = []
    lines.append("# 서버 처리 모델 비교 리포트")
    lines.append("")
    lines.append("## 이 문서를 처음 보는 분을 위한 안내")
    lines.append("")
    lines.append("이 문서는 같은 DB API 서버를 세 가지 방식으로 운영했을 때 어떤 차이가 나는지 비교한 결과입니다.")
    lines.append("")
    lines.append("- 직렬 서버: 요청을 한 번에 하나씩 처리합니다. 창구가 1개인 상황에 가깝습니다.")
    lines.append("- 스레드풀 서버: 미리 만들어 둔 worker thread들이 요청을 나눠 처리합니다. 현재 팀 구현과 같은 방식입니다.")
    lines.append("- 요청당 스레드 서버: 요청이 들어올 때마다 새 thread를 만들어 처리합니다.")
    lines.append("")
    lines.append("용어 설명:")
    lines.append("- 처리량(req/s): 1초에 성공적으로 끝낸 요청 수입니다. 높을수록 좋습니다.")
    lines.append("- 평균 지연(avg): 전체 요청의 평균 응답 시간입니다.")
    lines.append("- p95 지연: 느린 쪽 5% 구간이 시작되는 응답 시간입니다. 사용자가 체감하는 '가끔 너무 느린 순간'을 보는 지표입니다.")
    lines.append("- 503: 서버가 현재 감당할 수 없어 요청을 거절한 횟수입니다.")
    lines.append("- 오류(error): 503 외의 실패입니다. 연결 실패, timeout 같은 경우가 여기에 들어갑니다.")
    lines.append("- RSS: 서버가 실제로 점유한 메모리 크기입니다. 이 보고서에서는 읽기 쉽게 MB로 환산해 표기했습니다.")
    lines.append("- Voluntary / Involuntary context switch: CPU가 thread를 바꿔 끼우는 횟수로, 높을수록 운영 비용이 커질 수 있습니다.")
    lines.append("")
    lines.append("## 실험 원칙")
    lines.append("")
    lines.append("- HTTP API, SQL 처리 로직, DB 파일, lock 정책은 모두 동일하게 유지했습니다.")
    lines.append("- 바뀐 것은 `요청을 worker에게 어떻게 넘기느냐`뿐입니다.")
    lines.append("- 같은 머신에서 같은 데이터셋과 같은 요청 패턴으로 반복 측정했습니다.")
    lines.append(f"- 반복 횟수: 각 조합 `{args.repeats}회`")
    lines.append("")
    lines.append("## 핵심 결론")
    lines.append("")
    for highlight in build_key_findings(rows):
        lines.append(f"- {highlight}")
    lines.append("")
    lines.append("## 대표 장면 요약")
    lines.append("")
    lines.extend(build_snapshot_table(rows))
    lines.append("")
    lines.append("## 그래프")
    lines.append("")
    lines.append(build_mermaid_throughput(rows))
    lines.append("")
    lines.append(build_mermaid_p95(rows))
    lines.append("")
    lines.extend(build_phase_tables(rows, args.phases))
    lines.append("## 해석")
    lines.append("")
    lines.append("- 이번 환경에서는 병렬 처리의 이점이 `항상 큰 처리량 증가`로 나타나지는 않았습니다. 기본 부하에서는 직렬이 비슷하거나 더 빠른 구간도 적지 않았습니다.")
    lines.append("- 다만 병렬 모델은 읽기/혼합 workload에서 p95를 낮추거나, burst 상황에서 더 나은 응답 품질을 주는 방향으로 강점을 보였습니다.")
    lines.append("- 요청당 스레드 방식은 overload에서 빠른 처리량을 내는 순간도 있었지만, 메모리와 context switch 비용이 크게 증가했습니다.")
    lines.append("- 스레드풀은 같은 overload 상황에서 요청당 스레드보다 메모리 사용량과 context switch를 낮게 유지하면서, 유사한 p95를 제공했습니다.")
    lines.append("- 이번 비교에는 queue의 최대 순간 점유율(high-water mark) 계측은 넣지 않았습니다. 대신 `503`, 메모리, context switch를 함께 보며 안정성을 판단했습니다.")
    lines.append("")
    report_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    args = parse_args()
    root_dir = Path(args.root_dir).resolve()
    tests_dir = Path(__file__).resolve().parent

    server_bins = {
        "serial": (root_dir / args.serial_bin).resolve(),
        "thread_pool": (root_dir / args.thread_pool_bin).resolve(),
        "thread_per_request": (root_dir / args.thread_per_request_bin).resolve(),
    }

    ensure_server_binaries(root_dir, server_bins)

    scenarios = build_scenarios(args.phases)
    if args.scenario_ids:
        requested_ids = set(args.scenario_ids)
        scenarios = [scenario for scenario in scenarios if scenario.scenario_id in requested_ids]
        if not scenarios:
            raise SystemExit("no scenarios matched --scenario-ids")
    dataset_cache = {
        dataset_rows: generate_dataset(root_dir / "reports" / "model_compare_datasets", dataset_rows)
        for dataset_rows in sorted({scenario.dataset_rows for scenario in scenarios})
    }

    samples: List[ComparisonSample] = []
    total_runs = len(scenarios) * len(server_bins) * args.repeats
    completed_runs = 0

    for scenario in scenarios:
        base_dataset = dataset_cache[scenario.dataset_rows]
        for model, server_bin in server_bins.items():
            for repeat_index in range(1, args.repeats + 1):
                completed_runs += 1
                print(
                    f"[{completed_runs}/{total_runs}] {scenario.scenario_id} | {model} | repeat {repeat_index}",
                    flush=True,
                )
                sample = run_single_sample(
                    model=model,
                    server_bin=server_bin,
                    scenario=scenario,
                    base_dataset=base_dataset,
                    repeat_index=repeat_index,
                    timeout_seconds=args.timeout_seconds,
                    async_timeout_seconds=args.async_timeout_seconds,
                )
                samples.append(sample)

    summaries = aggregate_samples(samples)
    raw_csv_path = (root_dir / args.raw_csv_out).resolve()
    summary_csv_path = (root_dir / args.summary_csv_out).resolve()
    report_path = (root_dir / args.report_out).resolve()

    write_raw_csv(samples, raw_csv_path)
    write_summary_csv(summaries, summary_csv_path)
    write_report(report_path, summaries, scenarios, args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
