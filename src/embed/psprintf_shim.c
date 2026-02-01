#include "postgres.h"

#include "utils/memutils.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static void *
workadb_alloc(size_t size)
{
	MemoryContext context = CurrentMemoryContext;

	if (context == NULL)
		context = TopMemoryContext;
	if (context == NULL)
	{
		fprintf(stderr, "psprintf called before memory context init\n");
		exit(EXIT_FAILURE);
	}

	return MemoryContextAlloc(context, size ? size : 1);
}

static void
workadb_free(void *ptr)
{
	if (!ptr)
		return;

	pfree(ptr);
}

char *
psprintf(const char *fmt, ...)
{
	int save_errno = errno;
	size_t len = 128;

	for (;;)
	{
		char *result;
		va_list args;
		size_t newlen;

		result = (char *) workadb_alloc(len);

		errno = save_errno;
		va_start(args, fmt);
		newlen = pvsnprintf(result, len, fmt, args);
		va_end(args);

		if (newlen < len)
			return result;

		workadb_free(result);
		len = newlen;
	}
}

size_t
pvsnprintf(char *buf, size_t len, const char *fmt, va_list args)
{
	int nprinted;

	nprinted = vsnprintf(buf, len, fmt, args);

	if (unlikely(nprinted < 0))
	{
		if (CurrentMemoryContext != NULL)
			elog(ERROR, "vsnprintf failed: %m with format string \"%s\"", fmt);
		fprintf(stderr, "vsnprintf failed: %m with format string \"%s\"\n", fmt);
		exit(EXIT_FAILURE);
	}

	if ((size_t) nprinted < len)
		return (size_t) nprinted;

	if (unlikely((size_t) nprinted > MaxAllocSize - 1))
	{
		if (CurrentMemoryContext != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("out of memory")));
		fprintf(stderr, "out of memory\n");
		exit(EXIT_FAILURE);
	}

	return nprinted + 1;
}
