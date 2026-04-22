#!/usr/bin/env python3

import argparse
import shutil
import tempfile
from pathlib import Path
from typing import List

from run_api_server_benchmarks import (
    BenchmarkRow,
    BenchmarkSummaryRow,
    aggregate_rows,
    append_reader_guide,
    ensure_server_built,
    format_mean_stddev,
    format_workload_label,
    generate_dataset,
    run_single_benchmark,
    write_csv,
    write_raw_csv,
)


DEFAULT_DATASET_SIZES = [100_000, 1_000_000]
DEFAULT_WORKERS = [4, 8, 16]
DEFAULT_QUEUES = [128, 256, 512]
DEFAULT_WORKLOADS = ["mixed", "select-only"]
DEFAULT_CONCURRENCY_LEVELS = [256, 512, 1024]
DEFAULT_REPEATS = 10


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run high-concurrency stress sweeps for mini_db_server.")
    parser.add_argument("--server-bin", default="./mini_db_server")
    parser.add_argument("--root-dir", default=".")
    parser.add_argument("--dataset-sizes", nargs="+", type=int, default=DEFAULT_DATASET_SIZES)
    parser.add_argument("--workers", nargs="+", type=int, default=DEFAULT_WORKERS)
    parser.add_argument("--queues", nargs="+", type=int, default=DEFAULT_QUEUES)
    parser.add_argument("--workloads", nargs="+", default=DEFAULT_WORKLOADS)
    parser.add_argument("--concurrency-levels", nargs="+", type=int, default=DEFAULT_CONCURRENCY_LEVELS)
    parser.add_argument("--repeats", type=int, default=DEFAULT_REPEATS)
    parser.add_argument("--csv-out", default="reports/data/api_server_stress_results.csv")
    parser.add_argument("--raw-csv-out", default="reports/data/api_server_stress_samples.csv")
    parser.add_argument("--report-out", default="reports/details/api_server_stress_report.md")
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


def write_report(rows: List[BenchmarkSummaryRow],
                 report_path: Path,
                 summary_csv_path: Path,
                 raw_csv_path: Path,
                 args: argparse.Namespace) -> None:
    best_rows = choose_best_rows(rows)
    lines = []
    lines.append("# mini_db_server 고동시성 스트레스 리포트")
    lines.append("")
    append_reader_guide(lines, "stress")
    lines.append("## 측정 조건")
    lines.append("")
    lines.append(f"- 데이터셋 크기: `{', '.join(str(size) for size in args.dataset_sizes)}`행")
    lines.append(f"- 워커 수 후보: `{', '.join(str(worker) for worker in args.workers)}`")
    lines.append(f"- 큐 크기 후보: `{', '.join(str(queue) for queue in args.queues)}`")
    lines.append(f"- 부하 종류: `{', '.join(format_workload_label(workload) for workload in args.workloads)}`")
    lines.append(f"- 동시성 수준: `{', '.join(str(level) for level in args.concurrency_levels)}`")
    lines.append(f"- 반복 횟수: 각 조합당 `{args.repeats}`회")
    lines.append("- 각 run은 `총 요청 수 = 동시 클라이언트 수`로 맞춰, 해당 동시성 수준의 순간 버스트만 측정했습니다.")
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
    lines.append("## 해석")
    lines.append("")
    lines.append("- 이 스윕은 버스트 동시성만 따로 분리해서 봅니다. 일반 벤치 리포트와 달리 각 run의 요청 수는 동시 클라이언트 수와 같습니다.")
    lines.append("- 각 조합을 10회 반복 측정했고, 성공/실패 수와 지연 시간 모두 평균과 표준편차를 같이 표시합니다.")
    lines.append("- `혼합 (mixed)` 부하는 read lock 경로와 write lock 경로를 동시에 타기 때문에 우선적으로 해석해야 하는 지표입니다.")
    lines.append("- `오류`가 0이 아니면, 포화 상태에서 클라이언트가 JSON 응답 자체를 받지 못하고 실패한 경우가 있었다는 뜻입니다. 이는 `503`만 세는 것보다 더 엄격한 기준입니다.")
    lines.append("")
    lines.append("## 전체 결과")
    lines.append("")
    for dataset_rows in args.dataset_sizes:
        lines.append(f"### 데이터셋 {dataset_rows}행")
        lines.append("")
        for workload in args.workloads:
            subset = [
                row for row in rows
                if row.dataset_rows == dataset_rows and row.workload == workload
            ]
            subset.sort(key=lambda row: (row.total_requests, row.workers, row.queue_size))
            lines.append(f"#### {format_workload_label(workload)}")
            lines.append("")
            lines.append("| 동시 클라이언트 수 | 워커 수 | 큐 크기 | 반복 수 | 성공 평균+-표준편차 | 503 평균+-표준편차 | 오류 평균+-표준편차 | 평균 ms 평균+-표준편차 | p95 ms 평균+-표준편차 | 최대 ms 평균+-표준편차 |")
            lines.append("| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |")
            for row in subset:
                lines.append(
                    f"| {row.total_requests} | {row.workers} | {row.queue_size} | {row.repeats} | "
                    f"{format_mean_stddev(row.success_mean, row.success_stddev, 1)} | "
                    f"{format_mean_stddev(row.rejected_mean, row.rejected_stddev, 1)} | "
                    f"{format_mean_stddev(row.error_mean, row.error_stddev, 1)} | "
                    f"{format_mean_stddev(row.avg_ms_mean, row.avg_ms_stddev)} | "
                    f"{format_mean_stddev(row.p95_ms_mean, row.p95_ms_stddev)} | "
                    f"{format_mean_stddev(row.max_ms_mean, row.max_ms_stddev)} |"
                )
            lines.append("")
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> None:
    args = parse_args()
    root_dir = Path(args.root_dir).resolve()
    server_bin = (root_dir / args.server_bin).resolve() if not Path(args.server_bin).is_absolute() else Path(args.server_bin)
    csv_path = (root_dir / args.csv_out).resolve()
    raw_csv_path = (root_dir / args.raw_csv_out).resolve()
    report_path = (root_dir / args.report_out).resolve()

    ensure_server_built(server_bin, root_dir)

    datasets_root = Path(tempfile.mkdtemp(prefix="mini_db_stress_datasets_", dir="/tmp"))
    samples: List[BenchmarkRow] = []
    try:
        dataset_dirs = {size: generate_dataset(datasets_root, size) for size in args.dataset_sizes}
        for dataset_rows in args.dataset_sizes:
            for workload in args.workloads:
                for concurrency in args.concurrency_levels:
                    total_requests = concurrency
                    for workers in args.workers:
                        for queue_size in args.queues:
                            for repeat_index in range(1, args.repeats + 1):
                                row = run_single_benchmark(
                                    server_bin=server_bin,
                                    base_dataset=dataset_dirs[dataset_rows],
                                    dataset_rows=dataset_rows,
                                    workers=workers,
                                    queue_size=queue_size,
                                    workload=workload,
                                    total_requests=total_requests,
                                    concurrency=concurrency,
                                    repeat_index=repeat_index,
                                )
                                samples.append(row)
                                print(
                                    f"[stress] rows={row.dataset_rows} workload={row.workload} cc={concurrency} "
                                    f"workers={row.workers} queue={row.queue_size} repeat={row.repeat_index}/{args.repeats} "
                                    f"success={row.success_requests}/{row.total_requests} 503={row.rejected_requests} "
                                    f"errors={row.error_requests} avg_ms={row.avg_ms:.3f} p95_ms={row.p95_ms:.3f}",
                                    flush=True,
                                )

        summary_rows = aggregate_rows(samples)
        write_raw_csv(samples, raw_csv_path)
        write_csv(summary_rows, csv_path)
        write_report(summary_rows, report_path, csv_path, raw_csv_path, args)
        print(f"[done] wrote summary CSV to {csv_path}")
        print(f"[done] wrote raw CSV to {raw_csv_path}")
        print(f"[done] wrote report to {report_path}")
    finally:
        shutil.rmtree(datasets_root, ignore_errors=True)


if __name__ == "__main__":
    main()
