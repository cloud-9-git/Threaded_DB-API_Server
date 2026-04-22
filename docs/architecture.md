# Architecture

전체 구조는 아래 흐름입니다.

```text
client(http/js)
-> server
-> queue
-> thread pool
-> SQL + B+ tree mini DB
-> server
-> client
```

## 요청 흐름

1. 클라이언트가 HTTP 요청을 보낸다.
2. `src/http.c`가 요청 line, header, body를 읽는다.
3. `POST /sql`이면 JSON에서 `"sql"` 문자열을 꺼낸다.
4. HTTP thread가 `Job`을 만들어 `Queue`에 넣는다.
5. worker thread가 `q_pop()`으로 작업을 꺼낸다.
6. worker가 `db_exec()`를 호출한다.
7. `db_exec()`는 SQL 종류를 보고 read/write lock을 잡는다.
8. books row 저장소와 B+Tree id 인덱스를 조회하거나 수정한다.
9. worker가 `wait_ms`, `work_ms`, `total_ms`를 붙여 JSON 응답을 쓴다.

## 파일 역할

- `src/server_main.c`: port, worker 수, DB, pool 초기화
- `src/http.c`: socket accept, HTTP parse, JSON extract, route
- `src/queue.c`: `QUEUE_MAX=1024` ring buffer
- `src/pool.c`: worker 생성, job 처리, 통계
- `src/db_api.c`: 서버가 호출하는 유일한 SQL entry point
- `src/bptree.c`: 기존 B+Tree 인덱스 구현
- `bench/bench.js`: worker 수별 READ/WRITE/MIXED 측정
- `bench/chart.html`: `bench/result.json` 시각화

## Thread 구조

main thread는 accept와 요청 파싱을 맡습니다.

worker thread는 DB 실행과 응답 작성을 맡습니다.

이 구조는 요청 접수와 DB 처리 시간을 분리해서 queue 대기 시간과 실제 작업 시간을 각각 측정할 수 있게 합니다.
