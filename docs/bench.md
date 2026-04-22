# Benchmark

## 명령

대규모 과제 실행:

```bash
make bench COUNT=1000000
```

빠른 확인:

```bash
make bench COUNT=10000
```

직접 실행:

```bash
node bench/bench.js --workers 1,2,4,8,16,32 --count 1000000 --conc 128
```

## 측정 모드

- `write`: INSERT만 실행
- `read`: seed row를 넣고 SELECT만 실행
- `mixed`: INSERT와 SELECT를 섞어서 실행

## 결과 형식

`bench/result.json`:

```json
[
  {
    "workers": 1,
    "mode": "write",
    "count": 1000000,
    "conc": 128,
    "total_ms": 100000,
    "avg_wait_ms": 5.1,
    "avg_work_ms": 1.2,
    "qps": 10000
  }
]
```

## 차트

```text
bench/chart.html
```

또는 서버 실행 중:

```text
http://127.0.0.1:8080/chart
```

차트 항목:

- worker 개수별 total time
- worker 개수별 avg wait time
- worker 개수별 avg work time
- worker 개수별 QPS

## 실제 검증 결과

시간을 통제하기 위해 작은 COUNT로 검증했습니다.

```bash
make bench COUNT=30 BENCH_WORKERS=1,2 CONC=4
```

결과 파일:

```text
bench/result.json
```

1,000,000건 벤치마크는 명령을 제공했지만 현재 작업 중 실제로 실행하지 않았습니다.
