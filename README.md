# Threaded DB API Server

C 언어로 구현한 미니 DBMS API 서버입니다.

외부 클라이언트는 HTTP/JSON으로 SQL을 보내고, 서버는 작업 큐와 thread pool을 거쳐 books 미니 DB를 실행한 뒤 JSON으로 결과를 돌려줍니다.

```text
client(http/js)
-> server
-> queue
-> thread pool
-> SQL + B+ tree mini DB
-> server
-> client
```

## 구현 범위

- POSIX socket 기반 HTTP 서버
- JSON 요청/응답
- 고정 크기 ring buffer 작업 큐
- pthread 기반 worker thread pool
- 기존 `src/bptree.c` B+Tree를 id 인덱스로 재사용
- `db_exec()` wrapper
- books 테이블 전용 SQL
- READ / WRITE / MIXED 벤치마크
- worker 수별 benchmark runner
- Canvas 기반 chart
- 단위 테스트, API 테스트, 엣지 케이스 테스트
- GitHub Actions CI

## 지원 SQL

```sql
CREATE TABLE books;
INSERT INTO books VALUES (1, 'title', 'author', 2024);
SELECT * FROM books WHERE id = 1;
SELECT * FROM books;
DELETE FROM books WHERE id = 1;
```

`title`, `author`에는 공백이 들어갈 수 있습니다.

## 빌드

```bash
make
```

생성 바이너리:

- `server`: HTTP/JSON DB API 서버
- `sql_processor`: 기존 CLI SQL 처리기

## 실행

```bash
make run WORKERS=8 PORT=8080
```

직접 실행:

```bash
./server --port 8080 --workers 8
```

worker 기본값은 CPU core 수의 2배입니다.

## API

Health:

```bash
curl http://127.0.0.1:8080/health
```

SQL:

```bash
curl -X POST http://127.0.0.1:8080/sql \
  -H 'Content-Type: application/json' \
  -d '{"sql":"INSERT INTO books VALUES (1, '\''C Book'\'', '\''Kim'\'', 2024);"}'
```

Stats:

```bash
curl http://127.0.0.1:8080/stats
```

Bench endpoint:

```bash
curl -X POST http://127.0.0.1:8080/bench \
  -H 'Content-Type: application/json' \
  -d '{"mode":"write","count":1000}'
```

Chart:

```text
http://127.0.0.1:8080/chart
```

## 테스트

```bash
make test
make api-test
```

검증 항목:

- B+Tree insert/search/duplicate
- Queue push/pop/full/empty
- SQL create/insert/select/delete
- 빈 SQL, 긴 SQL, 잘못된 SQL
- 없는 id 검색
- 중복 id INSERT
- DELETE 없는 id
- title/author 공백 포함
- worker 1개 API 처리
- 동시 SELECT 100개 이상
- bad JSON, bad SQL

## 벤치마크

과제용 대규모 실행:

```bash
make bench COUNT=1000000
```

빠른 확인:

```bash
make bench COUNT=10000
```

직접 옵션:

```bash
node bench/bench.js --workers 1,2,4,8,16,32 --count 1000000 --conc 128
```

결과:

- `bench/result.json`
- `bench/chart.html`

## 구조

주요 파일:

- `src/server_main.c`: 서버 실행 옵션과 초기화
- `src/http.c`: HTTP 파싱, JSON 추출, 라우팅
- `src/queue.c`: 고정 크기 작업 큐
- `src/pool.c`: worker thread pool
- `src/db_api.c`: books SQL wrapper와 B+Tree 인덱스 연결
- `src/bptree.c`: 기존 B+Tree 구현
- `bench/bench.js`: worker별 benchmark
- `bench/chart.html`: 결과 차트

## 한계점

- 완전한 SQL 엔진이 아니라 과제에 필요한 books SQL 부분집합만 지원합니다.
- HTTP 요청은 한 연결당 한 요청만 처리합니다.
- chunked request는 지원하지 않습니다.
- DB는 서버 프로세스 메모리에 저장됩니다.
- `SELECT * FROM books;` 응답이 너무 커지면 JSON을 안전하게 줄이고 `truncated:true`를 표시합니다.
- 기존 B+Tree에 delete 함수가 없어 DELETE 시 살아있는 row 기준으로 인덱스를 재빌드합니다.
