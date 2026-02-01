#include "postgres.h"

#include <errno.h>
#include <string.h>

#include "libpq/libpq-be.h"
#include "libpq/workadb_io.h"

typedef struct WorkadbIoState
{
	const uint8_t *in_bytes;
	size_t		in_len;
	size_t		in_pos;
	uint8_t    *out_bytes;
	size_t		out_cap;
	size_t		out_len;
	bool		enabled;
} WorkadbIoState;

static WorkadbIoState workadb_io_state;

void
workadb_io_enable(bool enable)
{
	workadb_io_state.enabled = enable;
}

bool
workadb_io_is_enabled(void)
{
	return workadb_io_state.enabled;
}

void
workadb_io_set_input(const uint8_t *bytes, size_t len)
{
	workadb_io_state.in_bytes = bytes;
	workadb_io_state.in_len = len;
	workadb_io_state.in_pos = 0;
}

void
workadb_io_set_output(uint8_t *bytes, size_t capacity)
{
	workadb_io_state.out_bytes = bytes;
	workadb_io_state.out_cap = capacity;
	workadb_io_state.out_len = 0;
}

size_t
workadb_io_output_len(void)
{
	return workadb_io_state.out_len;
}

void
workadb_io_reset(void)
{
	workadb_io_state.in_bytes = NULL;
	workadb_io_state.in_len = 0;
	workadb_io_state.in_pos = 0;
	workadb_io_state.out_bytes = NULL;
	workadb_io_state.out_cap = 0;
	workadb_io_state.out_len = 0;
	workadb_io_state.enabled = false;
}

ssize_t
workadb_io_read(Port *port, void *ptr, size_t len)
{
	size_t		remaining;
	size_t		to_copy;

	(void) port;

	if (!workadb_io_state.enabled)
	{
		errno = ENOTCONN;
		return -1;
	}

	if (workadb_io_state.in_pos >= workadb_io_state.in_len)
		return 0;

	remaining = workadb_io_state.in_len - workadb_io_state.in_pos;
	to_copy = (len < remaining) ? len : remaining;

	memcpy(ptr, workadb_io_state.in_bytes + workadb_io_state.in_pos, to_copy);
	workadb_io_state.in_pos += to_copy;

	return (ssize_t) to_copy;
}

ssize_t
workadb_io_write(Port *port, const void *ptr, size_t len)
{
	size_t		remaining;
	size_t		to_copy;

	(void) port;

	if (!workadb_io_state.enabled)
	{
		errno = ENOTCONN;
		return -1;
	}

	if (!workadb_io_state.out_bytes || workadb_io_state.out_cap == 0)
	{
		errno = ENOSPC;
		return -1;
	}

	remaining = workadb_io_state.out_cap - workadb_io_state.out_len;
	if (remaining == 0)
	{
		errno = ENOSPC;
		return -1;
	}

	to_copy = (len < remaining) ? len : remaining;
	memcpy(workadb_io_state.out_bytes + workadb_io_state.out_len, ptr, to_copy);
	workadb_io_state.out_len += to_copy;

	return (ssize_t) to_copy;
}
