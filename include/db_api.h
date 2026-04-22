#ifndef DB_API_H
#define DB_API_H

#define DB_SQL_MAX 2048
#define DB_OUT_MAX 65536

int db_init(void);
void db_free(void);
int db_exec(const char *sql, char *out, int max);
int db_run_bench(const char *mode, long count, int workers, char *out, int max);
int db_is_read_sql(const char *sql);

#endif
