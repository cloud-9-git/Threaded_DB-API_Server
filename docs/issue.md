# Threaded DB API Server 구현

## 목표

C 언어로 미니 DBMS API 서버를 구현한다.

```text
client(http/js)
-> server
-> queue
-> thread pool
-> SQL + B+ tree mini DB
-> server
-> client
```

## 구현 항목

- C HTTP/JSON API 서버
- 작업 큐
- Thread Pool
- Worker Thread 병렬 SQL 처리
- 기존 B+Tree 인덱스 재사용
- books 테이블 SQL 처리
- READ / WRITE / MIXED 벤치마크
- worker thread 개수별 wait/work/total/QPS 비교
- JS benchmark runner
- Canvas chart
- 단위 테스트
- API 테스트
- 엣지 케이스 테스트
- GitHub Actions CI

## 완료 조건

- `make` 성공
- `make test` 성공
- `make api-test` 성공
- `make bench COUNT=1000000` 명령 제공
- `bench/result.json` 생성
- `bench/chart.html` 제공
