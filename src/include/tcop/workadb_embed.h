#ifndef WORKADB_EMBED_H
#define WORKADB_EMBED_H

#include <stdbool.h>
#include <stddef.h>

#include "workadb.h"

bool workadb_backend_start(const char *data_dir,
						   const char *database_name,
						   const char *user_name,
						   char *errbuf,
						   size_t errlen);
bool workadb_backend_is_started(void);
bool workadb_backend_exec(const char *sql, char *errbuf, size_t errlen);
bool workadb_backend_exec_sql(const uint8_t *sql_bytes,
							  size_t sql_len,
							  uint8_t *out,
							  size_t out_cap,
							  size_t *out_len,
							  int64_t *rows,
							  char *errbuf,
							  size_t errlen);
bool workadb_backend_exec_sql_params(const uint8_t *sql_bytes,
									 size_t sql_len,
									 const wepg_param *params,
									 size_t param_count,
									 uint8_t *out,
									 size_t out_cap,
									 size_t *out_len,
									 int64_t *rows,
									 char *errbuf,
									 size_t errlen);
void workadb_backend_shutdown(void);

int workadb_initdb_run(int argc, char *argv[]);

void workadb_exec_simple_query(const char *sql);

#endif
