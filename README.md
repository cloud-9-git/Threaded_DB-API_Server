# Threaded DB API Server

기존 C 기반 SQL 엔진을 유지한 채, 그 앞에 최소 HTTP/JSON 서버를 얹은 프로젝트다.

현재 레포는 두 실행 파일을 제공한다.

- `sql_processor`: 파일 기반 CLI SQL 실행기
- `mini_db_server`: thread pool + bounded queue 기반 HTTP API 서버

지원 SQL 범위는 현재 엔진 기준으로 `SELECT`, `INSERT`다. API 서버는 요청당 SQL 1문장만 허용한다.

## Build

```bash
make
```

빌드 결과:

- `./sql_processor`
- `./mini_db_server`
- `build/test_*`
- `build/benchmark_bptree`

## Test

기존 엔진 테스트와 API 서버 테스트를 함께 실행한다.

```bash
make test
```

추가 확인용 기본 수용 기준:

```bash
./sql_processor -d db -f queries/multi_statements.sql
```

## CLI Engine

SQL 파일을 읽어 기존 엔진으로 실행한다.

```bash
./sql_processor -d db -f queries/multi_statements.sql
```

예시 SQL:

```sql
INSERT INTO users VALUES ('Alice', 20);
SELECT * FROM users WHERE id = 1;
```

## API Server

서버 실행:

```bash
./mini_db_server -d db -p 8080 -t 4 -q 64
```

지원 옵션:

| 옵션 | 의미 | 기본값 |
| --- | --- | --- |
| `-d`, `--db` | DB 디렉터리 | 필수 |
| `-p`, `--port` | 포트 | `8080` |
| `-t`, `--threads` | worker 수 | `4` |
| `-q`, `--queue-size` | queue capacity | `64` |
| `-h`, `--help` | 도움말 | 없음 |

### Health Check

```bash
curl -s http://127.0.0.1:8080/health | jq
```

응답:

```json
{
  "success": true,
  "service": "mini_db_server"
}
```

### Query API

INSERT:

```bash
curl -s -X POST http://127.0.0.1:8080/query \
  -H "Content-Type: application/json" \
  --data '{"sql":"INSERT INTO users VALUES ('\''Alice'\'', 20);"}' | jq
```

SELECT:

```bash
curl -s -X POST http://127.0.0.1:8080/query \
  -H "Content-Type: application/json" \
  --data '{"sql":"SELECT * FROM users WHERE id = 1;"}' | jq
```

SELECT 성공 응답 예:

```json
{
  "success": true,
  "type": "select",
  "used_index": true,
  "row_count": 1,
  "columns": ["id", "name", "age"],
  "rows": [["1", "Alice", "20"]]
}
```

INSERT 성공 응답 예:

```json
{
  "success": true,
  "type": "insert",
  "affected_rows": 1,
  "generated_id": 1
}
```

에러 응답 예:

```json
{
  "success": false,
  "error_code": "MULTI_STATEMENT_NOT_ALLOWED",
  "message": "only one SQL statement is allowed"
}
```

## API Rules

- `GET /health`
- `POST /query`
- JSON body의 top-level `sql` 문자열만 읽는다
- 요청당 SQL 1문장만 허용한다
- `SELECT`는 preload 후 read lock으로 실행한다
- `INSERT`는 write lock으로 실행한다
- queue가 가득 차면 `503`과 `QUEUE_FULL`을 반환한다

## Error Codes

주요 응답 코드:

- `INVALID_JSON`
- `MISSING_SQL_FIELD`
- `EMPTY_QUERY`
- `MULTI_STATEMENT_NOT_ALLOWED`
- `UNSUPPORTED_QUERY`
- `SQL_PARSE_ERROR`
- `SCHEMA_ERROR`
- `STORAGE_ERROR`
- `INDEX_ERROR`
- `EXECUTION_ERROR`
- `QUEUE_FULL`
- `BAD_REQUEST`
- `NOT_FOUND`
- `METHOD_NOT_ALLOWED`
- `PAYLOAD_TOO_LARGE`
- `INTERNAL_ERROR`

## Benchmark

API 서버용 benchmark 스크립트는 수동 실행용이다.

```bash
sh tests/bench_api_server.sh
```

기본 측정 조합:

- workers: `2`, `4`, `8`
- queue size: `32`, `64`, `128`
- workload: `select-only`, `insert-only`, `mixed`

결과 파일:

```text
build/bench_api_server_results.tsv
```

컬럼:

```text
workers	queue_size	workload	total_requests	success_requests	rejected_requests	total_ms	avg_ms	p95_ms
```

필요하면 환경 변수로 요청 수와 seed row 수를 조절할 수 있다.

```bash
REQUESTS_PER_RUN=120 SEED_ROWS=5000 sh tests/bench_api_server.sh
```

## Notes

- 기존 `sql_processor` 동작은 계속 유지된다.
- API 서버는 full HTTP/1.1, chunked body, SQL batch, `UPDATE`, `DELETE`, `JOIN`, transaction을 구현하지 않는다.
- 테스트와 benchmark 스크립트는 `curl`, `jq`, `python3`, `xargs`가 있는 환경을 전제로 한다.
