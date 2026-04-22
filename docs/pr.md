# feat: threaded DB API server

## Summary

- Added C HTTP/JSON API server with POSIX sockets.
- Added fixed-size ring queue and pthread worker pool.
- Added books mini DB API through `db_exec()`.
- Reused existing B+Tree implementation as the id index.
- Added read/write locking with `pthread_rwlock_t`.
- Added `/health`, `/sql`, `/stats`, `/bench`, and `/chart`.
- Added Node.js benchmark runner and Canvas chart.
- Added unit, API, edge-case tests, and GitHub Actions CI.

## Validation

```text
make: success
make test: success
make api-test: success
make bench COUNT=30 BENCH_WORKERS=1,2 CONC=4: success
```

## Notes

- The server supports the required books SQL subset, not a full SQL grammar.
- The existing CLI `sql_processor` remains available.
- `make bench COUNT=1000000` is provided for the large assignment benchmark but was not run during this implementation pass.
