# Test

## 명령

```bash
make test
make api-test
```

## 단위 테스트

`make test`는 `tests/test_*.c`를 모두 빌드하고 실행합니다.

현재 포함된 주요 테스트:

- `tests/test_bptree.c`: 기존 B+Tree 대량 insert/search/split/duplicate
- `tests/test_btree.c`: 과제명에 맞춘 B+Tree 기본 insert/search/duplicate
- `tests/test_queue.c`: queue push/pop/full/empty
- `tests/test_sql.c`: books SQL create/insert/select/delete와 edge case
- 기존 lexer/parser/executor/runtime/storage 테스트

## API 테스트

`tests/api_test.sh`는 서버를 worker 1개로 띄운 뒤 curl로 검증합니다.

검증:

- `GET /health`
- `POST /sql` CREATE
- `POST /sql` INSERT
- `POST /sql` SELECT
- `POST /sql` DELETE
- `GET /stats`
- bad JSON
- bad SQL
- 동시에 SELECT 120개
- `POST /bench`

## 실제 실행 결과

2026-04-22 실행:

```text
make: 성공
make test: 성공
make api-test: 성공
```

로컬 sandbox에서는 포트 bind가 제한될 수 있어 API 테스트는 승인된 실행 환경에서 돌렸습니다.
