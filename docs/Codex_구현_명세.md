# Codex 구현 명세

## 1. 목적

이 문서는 Codex가 현재 레포지토리에서 바로 구현 작업을 시작할 수 있도록, 구현 범위와 규칙만 추려 놓은 실행용 명세다.

사람이 전체 흐름을 이해하려면 `docs/전체_흐름_이해_가이드.md`를 먼저 보고, 실제 구현 작업은 이 문서를 기준으로 진행한다.

핵심 목표:

1. 기존 `sql_processor`는 그대로 유지한다.
2. 새 실행 파일 `mini_db_server`를 추가한다.
3. HTTP/JSON으로 SQL 1문장을 받아 기존 SQL 엔진으로 실행한다.
4. Thread Pool + Bounded Queue + `pthread_rwlock_t`로 병렬성과 정합성을 동시에 확보한다.

## 2. 이번 작업의 최종 범위

### 반드시 구현할 것

1. `GET /health`
2. `POST /query`
3. JSON body에서 top-level `sql` 필드 추출
4. 기존 SQL 엔진 호출
5. 단일 SQL statement만 허용
6. Thread Pool + Bounded Queue
7. `SELECT preload + read lock`, `INSERT write lock`
8. `ExecResult` -> JSON 변환
9. API 테스트와 benchmark 스크립트

### 구현하지 않을 것

1. SQL batch
2. `/stats`, `/bench`, `/chart`
3. `UPDATE`, `DELETE`, `JOIN`
4. 트랜잭션, rollback
5. full HTTP/1.1
6. 완전한 JSON 파서

## 3. 기존 코드베이스 전제

이미 있는 SQL 엔진 모듈:

- `src/lexer.c`
- `src/parser.c`
- `src/executor.c`
- `src/runtime.c`
- `src/storage.c`
- `src/bptree.c`
- `src/result.c`

중요한 사실:

1. 실제 row 데이터는 `.data` 파일에 저장된다.
2. 스키마는 `.schema` 파일에서 읽는다.
3. `ExecutionContext`는 실행 중 `TableRuntime` cache를 보관한다.
4. `get_or_load_table_runtime()`는 lazy loading을 수행하므로 첫 `SELECT`도 shared state를 수정할 수 있다.

따라서 무잠금 SELECT는 금지한다.

## 4. 실행 파일과 빌드 규칙

### 기존 실행 파일 유지

아래 명령은 계속 성공해야 한다.

```bash
make
./sql_processor -d db -f queries/multi_statements.sql
make test
```

### 새 실행 파일

```bash
./mini_db_server -d db -p 8080 -t 4 -q 64
```

### Makefile 규칙

1. `sql_processor`와 `mini_db_server`를 모두 빌드해야 한다.
2. `src/main.c`와 `src/server_main.c`를 같은 타깃에 같이 링크하면 안 된다.
3. 서버 타깃에는 `-pthread`를 추가한다.
4. 필요하면 `-D_XOPEN_SOURCE=700`를 추가한다.

권장 구조:

```make
COMMON_SRCS = \
  src/utils.c \
  src/lexer.c \
  src/parser.c \
  src/schema.c \
  src/storage.c \
  src/runtime.c \
  src/executor.c \
  src/result.c \
  src/bptree.c

CLI_SRCS = \
  src/main.c \
  src/cli.c

SERVER_SRCS = \
  src/server_main.c \
  src/server.c \
  src/http.c \
  src/thread_pool.c \
  src/task_queue.c \
  src/db_api.c \
  src/json_parser.c \
  src/json_writer.c
```

## 5. 서버 CLI 규칙

지원 옵션:

| 옵션 | 의미 | 기본값 |
| --- | --- | --- |
| `-d`, `--db` | DB 디렉터리 | 필수 |
| `-p`, `--port` | 포트 | `8080` |
| `-t`, `--threads` | worker 수 | `4` |
| `-q`, `--queue-size` | queue capacity | `64` |
| `-h`, `--help` | 도움말 | 없음 |

규칙:

1. `-d`가 없으면 종료한다.
2. `-t`, `-q`는 기본값으로 시작하지만 benchmark 결과에 따라 바뀔 수 있다.
3. queue capacity는 실행 옵션으로 조절 가능해야 한다.

## 6. API 규칙

### 지원 엔드포인트

| Method | Path |
| --- | --- |
| `GET` | `/health` |
| `POST` | `/query` |

### `GET /health`

응답:

```json
{
  "success": true,
  "service": "mini_db_server"
}
```

### `POST /query`

요청 body:

```json
{
  "sql": "SELECT * FROM users WHERE id = 1;"
}
```

제한:

1. top-level `sql`만 읽는다.
2. 요청당 SQL 1문장만 허용한다.
3. 여러 statement가 들어오면 `MULTI_STATEMENT_NOT_ALLOWED`로 거절한다.

## 7. 최종 응답 JSON 규격

### SELECT 성공

```json
{
  "success": true,
  "type": "select",
  "used_index": true,
  "row_count": 1,
  "columns": ["id", "name", "age"],
  "rows": [["1", "kim", "25"]]
}
```

### INSERT 성공

```json
{
  "success": true,
  "type": "insert",
  "affected_rows": 1,
  "generated_id": 1
}
```

### 에러

```json
{
  "success": false,
  "error_code": "SQL_PARSE_ERROR",
  "message": "expected INTO after INSERT"
}
```

규칙:

1. `results: []` 배열은 만들지 않는다.
2. benchmark용 메타데이터는 응답 본문에 넣지 않는다.
3. `used_index`와 `generated_id`는 포함한다.

## 8. HTTP 상태 코드 및 에러 코드

### 상태 코드

| 상태 코드 | 의미 |
| --- | --- |
| `200` | 성공 |
| `400` | 잘못된 JSON, 빈 SQL, 문법 오류, 다중 statement |
| `404` | 잘못된 path |
| `405` | 잘못된 method |
| `413` | body 상한 초과 |
| `503` | queue 포화 |
| `500` | 내부 오류 |

### error_code

1. `INVALID_JSON`
2. `MISSING_SQL_FIELD`
3. `EMPTY_QUERY`
4. `MULTI_STATEMENT_NOT_ALLOWED`
5. `UNSUPPORTED_QUERY`
6. `SQL_PARSE_ERROR`
7. `SCHEMA_ERROR`
8. `STORAGE_ERROR`
9. `INDEX_ERROR`
10. `EXECUTION_ERROR`
11. `QUEUE_FULL`
12. `INTERNAL_ERROR`

## 9. 동시성 정책

### 최종 정책

| SQL 종류 | 정책 |
| --- | --- |
| `SELECT` | preload 후 read lock |
| `INSERT` | write lock |

### 이유

현재 `get_or_load_table_runtime()`는 첫 접근 시 아래 작업을 할 수 있다.

1. schema load
2. `.data` 파일 확인
3. B+Tree rebuild
4. `ExecutionContext.tables` append
5. `next_id` 계산

즉 첫 `SELECT`도 shared state를 수정할 수 있으므로, 아래 순서를 고정한다.

### SELECT 처리 규칙

```text
1. SQL tokenize
2. 정확히 1개의 statement로 parse
3. table_name 추출
4. write lock 획득
5. runtime_preload_table(&ctx, table_name, ...)
6. write lock 해제
7. read lock 획득
8. execute_statement(&ctx, &stmt, &result, ...)
9. read lock 해제
```

### INSERT 처리 규칙

```text
1. SQL tokenize
2. 정확히 1개의 statement로 parse
3. write lock 획득
4. execute_statement(&ctx, &stmt, &result, ...)
5. write lock 해제
```

## 10. Thread Pool / Queue 정책

### 초기 기본값

```text
accept thread: 1
worker threads: 4
queue capacity: 64
request body max: 8192 bytes
```

### 이 숫자는 최종값이 아니다

이 숫자는 초기 기준값이다. Codex는 구현 후 benchmark를 돌려 더 좋은 값이 있으면 최종값을 바꿀 수 있도록 해야 한다.

즉:

1. 기본값은 `4 / 64`
2. 실행 옵션으로 변경 가능해야 함
3. benchmark 후 최종값 재결정 가능해야 함

### benchmark 규칙

실행 조합:

1. worker: `2`, `4`, `8`
2. queue: `32`, `64`, `128`
3. workload:
   - `select-only`
   - `insert-only`
   - `mixed`

측정 항목:

1. 총 요청 수
2. 성공 수
3. 실패 수
4. `503` 비율
5. 총 처리 시간
6. 평균 응답 시간
7. p95 응답 시간

최종값 선택 규칙:

1. `mixed` workload를 최우선으로 본다.
2. `503` 비율이 너무 높으면 제외한다.
3. 성능 차이가 작으면 더 작은 worker/queue 조합을 선택한다.

## 11. 추가 / 수정 파일

### 새로 추가할 파일

```text
include/
  server.h
  http.h
  thread_pool.h
  task_queue.h
  db_api.h
  json_parser.h
  json_writer.h

src/
  server_main.c
  server.c
  http.c
  thread_pool.c
  task_queue.c
  db_api.c
  json_parser.c
  json_writer.c

tests/
  test_api_server.sh
  bench_api_server.sh
```

### 수정 가능한 파일

```text
Makefile
README.md
include/runtime.h
src/runtime.c
```

### 추가할 최소 공개 함수

```c
int runtime_preload_table(ExecutionContext *ctx,
                          const char *table_name,
                          char *errbuf,
                          size_t errbuf_size);
```

역할:

1. `get_or_load_table_runtime()` public wrapper
2. SELECT preload 단계에서 사용
3. 반드시 write lock 상태에서 호출

## 12. 파일별 책임

### `server_main.c`

1. CLI 옵션 파싱
2. `ServerConfig` 생성
3. `server_init()`
4. `server_run()`
5. `server_destroy()`

### `server.c`

1. listen socket 생성
2. accept loop
3. queue full 시 `503`
4. client task handler 연결

### `http.c`

1. request line / header / body 읽기
2. `Content-Length` 기반 body 읽기
3. JSON 응답 전송
4. 에러 응답 전송

### `task_queue.c`

1. bounded circular queue
2. 동적 capacity 지원
3. mutex/cond 관리
4. shutdown 처리

### `thread_pool.c`

1. worker thread 생성
2. task queue에서 작업 pop
3. task handler 호출
4. graceful shutdown

### `db_api.c`

1. HTTP 서버와 SQL 엔진 사이 adapter
2. SQL tokenize / parse / 단일 statement 검증
3. SELECT preload + read lock
4. INSERT write lock
5. `ExecResult` -> JSON 변환
6. `StatusCode` -> HTTP / error_code 매핑

### `json_parser.c`

1. JSON body에서 `"sql"` 문자열 추출

### `json_writer.c`

1. JSON 문자열 builder
2. 문자열 escape 처리

## 13. `db_api_execute_sql()` 구현 규칙

최종 흐름:

```text
1. 인자 검증
2. sql 빈 문자열 검사
3. tokenize_sql(sql)
4. parse_next_statement()로 첫 statement 파싱
5. 남은 토큰이 EOF인지 확인
   -> 아니면 MULTI_STATEMENT_NOT_ALLOWED
6. statement type 판별
7. SELECT면 preload + read lock 실행
8. INSERT면 write lock 실행
9. ExecResult를 JSON으로 직렬화
10. StatusCode를 HTTP status와 error_code로 매핑
11. heap JSON string 반환
```

## 14. 테스트 요구사항

### 기존 테스트 유지

```bash
make test
```

### 신규 API 테스트

`tests/test_api_server.sh` 최소 검증 항목:

1. `GET /health`
2. `POST /query` + `INSERT`
3. `POST /query` + `SELECT`
4. `used_index: true`
5. invalid JSON -> `400`
6. 다중 statement -> `400`
7. concurrent `SELECT`
8. concurrent `INSERT`
9. queue full -> `503`

### benchmark 스크립트

`tests/bench_api_server.sh` 최소 요구사항:

1. worker 수와 queue size 변경 가능
2. `select-only`, `insert-only`, `mixed` workload 지원
3. CSV 또는 TSV 결과 저장
4. 요약 출력 제공

예시 컬럼:

```text
workers,queue_size,workload,total_requests,success_requests,rejected_requests,total_ms,avg_ms,p95_ms
```

## 15. 완료 기준

1. `sql_processor`가 그대로 동작한다.
2. `mini_db_server`가 빌드된다.
3. `GET /health`가 동작한다.
4. `POST /query`가 SQL 1문장을 받아 실행한다.
5. 응답 JSON이 본 문서 규격과 일치한다.
6. batch SQL은 거절된다.
7. `SELECT`는 preload 후 read lock으로 실행된다.
8. `INSERT`는 write lock으로 실행된다.
9. queue full 시 `503`을 준다.
10. API 테스트와 benchmark 스크립트가 추가된다.

## 16. 구현 순서

1. `Makefile` 분리
2. `server_main.c`, `server.c` 뼈대 작성
3. `task_queue.c`, `thread_pool.c` 구현
4. `http.c`, `json_parser.c`, `json_writer.c` 구현
5. `db_api.c` 구현
6. `runtime_preload_table()` 추가
7. `GET /health` 연결
8. `POST /query` 연결
9. `test_api_server.sh` 작성
10. `bench_api_server.sh` 작성
11. benchmark 후 worker/queue 최종값 재평가
