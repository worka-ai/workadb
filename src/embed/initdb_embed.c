#define FRONTEND

#include <setjmp.h>

#include "postgres_fe.h"

int workadb_find_other_exec(const char *argv0, const char *target,
							const char *versionstr, char *retpath);

#define find_other_exec workadb_find_other_exec

static sigjmp_buf workadb_initdb_jmp;
static int workadb_initdb_exit_code = 0;

static void
workadb_initdb_exit(int code)
{
	workadb_initdb_exit_code = code;
	siglongjmp(workadb_initdb_jmp, 1);
}

int workadb_initdb_main(int argc, char *argv[]);
int workadb_initdb_run(int argc, char *argv[]);

int
workadb_initdb_run(int argc, char *argv[])
{
	workadb_initdb_exit_code = 0;
	if (sigsetjmp(workadb_initdb_jmp, 1) != 0)
		return workadb_initdb_exit_code;
	return workadb_initdb_main(argc, argv);
}

#define exit workadb_initdb_exit
#define main workadb_initdb_main

#include "bin/initdb/initdb.c"

#undef main
#undef exit
