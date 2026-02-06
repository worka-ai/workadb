#include "postgres_fe.h"

#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#include "common/fe_memutils.h"
#include "common/file_utils.h"
#include "common/logging.h"
#include "common/restricted_token.h"
#include "common/string.h"
#include "fe_utils/option_utils.h"
#include "fe_utils/string_utils.h"
#include "mb/pg_wchar.h"
#include "pqexpbuffer.h"
#include "port.h"
#include "pg_config_paths.h"

char *pg_strndup(const char *in, size_t size);
const char *select_default_timezone(const char *share_path);
extern const char *pg_encoding_to_char_private(int encoding);
extern int pg_valid_server_encoding_private(const char *name);
extern int pg_valid_server_encoding_id_private(int encoding);
int workadb_find_other_exec(const char *argv0, const char *target,
							const char *versionstr, char *retpath);

static void *
pg_malloc_internal(size_t size, int flags)
{
	void *tmp;

	if (size == 0)
		size = 1;
	tmp = malloc(size);
	if (!tmp)
	{
		if ((flags & MCXT_ALLOC_NO_OOM) == 0)
		{
			fprintf(stderr, "out of memory\n");
			exit(EXIT_FAILURE);
		}
		return NULL;
	}

	if ((flags & MCXT_ALLOC_ZERO) != 0)
		memset(tmp, 0, size);
	return tmp;
}

void *
pg_malloc(size_t size)
{
	return pg_malloc_internal(size, 0);
}

void *
pg_malloc0(size_t size)
{
	return pg_malloc_internal(size, MCXT_ALLOC_ZERO);
}

void *
pg_malloc_extended(size_t size, int flags)
{
	return pg_malloc_internal(size, flags);
}

void *
pg_realloc(void *ptr, size_t size)
{
	void *tmp;

	if (ptr == NULL && size == 0)
		size = 1;
	tmp = realloc(ptr, size);
	if (!tmp)
	{
		fprintf(stderr, "out of memory\n");
		exit(EXIT_FAILURE);
	}
	return tmp;
}

char *
pg_strdup(const char *in)
{
	size_t len;
	char *tmp;

	if (!in)
	{
		fprintf(stderr, "cannot duplicate null pointer\n");
		exit(EXIT_FAILURE);
	}

	len = strlen(in) + 1;
	tmp = (char *) pg_malloc(len);
	memcpy(tmp, in, len);
	return tmp;
}

char *
pg_strndup(const char *in, size_t size)
{
	size_t len;
	char *tmp;

	if (!in)
	{
		fprintf(stderr, "cannot duplicate null pointer\n");
		exit(EXIT_FAILURE);
	}

	len = strnlen(in, size);
	tmp = (char *) pg_malloc(len + 1);
	memcpy(tmp, in, len);
	tmp[len] = '\0';
	return tmp;
}

void
pg_free(void *ptr)
{
	free(ptr);
}

char *
simple_prompt(const char *prompt, bool echo)
{
	(void) prompt;
	(void) echo;
	return pg_strdup("");
}

char *
simple_prompt_extended(const char *prompt, bool echo, PromptInterruptContext *prompt_ctx)
{
	(void) prompt;
	(void) echo;
	(void) prompt_ctx;
	return pg_strdup("");
}

void
get_restricted_token(void)
{
}

void
sync_pgdata(const char *pg_data, int serverVersion,
			DataDirSyncMethod sync_method)
{
	(void) serverVersion;

	if (!pg_data || pg_data[0] == '\0')
		return;

	switch (sync_method)
	{
		case DATA_DIR_SYNC_METHOD_SYNCFS:
			{
#ifdef HAVE_SYNCFS
				int fd;

				fd = open(pg_data, O_RDONLY, 0);
				if (fd < 0)
				{
					pg_log_error("could not open file \"%s\": %m", pg_data);
					exit(EXIT_FAILURE);
				}

				if (syncfs(fd) < 0)
				{
					pg_log_error("could not synchronize file system for file \"%s\": %m", pg_data);
					(void) close(fd);
					exit(EXIT_FAILURE);
				}

				(void) close(fd);
				return;
#else
				pg_log_error("this build does not support sync method \"%s\"", "syncfs");
				exit(EXIT_FAILURE);
#endif
			}

		case DATA_DIR_SYNC_METHOD_FSYNC:
			{
				/* fall through to recursive fsync implementation below */
			}
			break;
	}

	/*
	 * Recursive fsync of all files and directories under pg_data.
	 *
	 * This intentionally avoids relying on fsync_fname()/file_utils.c because
	 * libworkadb links a minimal frontend subset. For initdb durability we
	 * fsync regular files and then fsync directories after their contents.
	 */
	{
		/* Simple post-order walk */
		struct workadb_sync_stack_item
		{
			char path[MAXPGPATH];
			bool entered;
			struct workadb_sync_stack_item *next;
		};

		struct workadb_sync_stack_item *stack = NULL;
		struct workadb_sync_stack_item *item;

		item = (struct workadb_sync_stack_item *) pg_malloc0(sizeof(*item));
		snprintf(item->path, sizeof(item->path), "%s", pg_data);
		item->entered = false;
		item->next = NULL;
		stack = item;

		while (stack)
		{
			struct workadb_sync_stack_item *cur = stack;
			stack = cur->next;

			if (!cur->entered)
			{
				DIR *dir;
				struct dirent *de;

				/* Push directory for fsync after children */
				cur->entered = true;
				cur->next = stack;
				stack = cur;

				dir = opendir(cur->path);
				if (!dir)
				{
					pg_log_error("could not open directory \"%s\": %m", cur->path);
					exit(EXIT_FAILURE);
				}

				while (errno = 0, (de = readdir(dir)) != NULL)
				{
					char subpath[MAXPGPATH];
					struct stat st;

					if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
						continue;

					if (snprintf(subpath, sizeof(subpath), "%s/%s", cur->path, de->d_name) >= (int) sizeof(subpath))
					{
						pg_log_error("path too long while syncing data directory");
						(void) closedir(dir);
						exit(EXIT_FAILURE);
					}

					if (lstat(subpath, &st) < 0)
					{
						pg_log_error("could not stat file \"%s\": %m", subpath);
						(void) closedir(dir);
						exit(EXIT_FAILURE);
					}

					if (S_ISDIR(st.st_mode))
					{
						struct workadb_sync_stack_item *child;
						child = (struct workadb_sync_stack_item *) pg_malloc0(sizeof(*child));
						snprintf(child->path, sizeof(child->path), "%s", subpath);
						child->entered = false;
						child->next = stack;
						stack = child;
					}
					else if (S_ISREG(st.st_mode))
					{
						int fd;

						fd = open(subpath, O_RDONLY, 0);
						if (fd < 0)
						{
							pg_log_error("could not open file \"%s\": %m", subpath);
							(void) closedir(dir);
							exit(EXIT_FAILURE);
						}

						if (fsync(fd) != 0)
						{
							pg_log_error("could not fsync file \"%s\": %m", subpath);
							(void) close(fd);
							(void) closedir(dir);
							exit(EXIT_FAILURE);
						}
						(void) close(fd);
					}
					/* ignore symlinks and special files */
				}

				if (errno)
				{
					pg_log_error("could not read directory \"%s\": %m", cur->path);
					(void) closedir(dir);
					exit(EXIT_FAILURE);
				}

				(void) closedir(dir);
			}
			else
			{
				int fd;
				int flags = O_RDONLY;

#ifdef O_DIRECTORY
				flags |= O_DIRECTORY;
#endif
				fd = open(cur->path, flags, 0);
				if (fd < 0)
				{
					pg_log_error("could not open directory \"%s\": %m", cur->path);
					exit(EXIT_FAILURE);
				}

				if (fsync(fd) != 0)
				{
					/*
					 * Some platforms/filesystems don't support fsync on
					 * directories; treat that as non-fatal.
					 */
					if (errno != EINVAL)
					{
						pg_log_error("could not fsync directory \"%s\": %m", cur->path);
						(void) close(fd);
						exit(EXIT_FAILURE);
					}
				}
				(void) close(fd);
				pg_free(cur);
			}
		}
	}
}

pqsigfunc
pqsignal_fe(int signum, pqsigfunc handler)
{
	return signal(signum, handler);
}

int
workadb_find_other_exec(const char *argv0, const char *target,
						const char *versionstr, char *retpath)
{
	(void) argv0;
	(void) versionstr;

	if (!retpath || !target)
		return -1;

	snprintf(retpath, MAXPGPATH, "%s/%s%s", PGBINDIR, target, EXE);
	return 0;
}

bool
option_parse_int(const char *optarg, const char *optname,
				 int min_range, int max_range, int *result)
{
	char *endptr;
	int val;

	errno = 0;
	val = strtoint(optarg, &endptr, 10);

	while (*endptr != '\0' && isspace((unsigned char) *endptr))
		endptr++;

	if (*endptr != '\0')
	{
		pg_log_error("invalid value \"%s\" for option %s", optarg, optname);
		return false;
	}

	if (errno == ERANGE || val < min_range || val > max_range)
	{
		pg_log_error("%s must be in range %d..%d", optname, min_range, max_range);
		return false;
	}

	if (result)
		*result = val;
	return true;
}

bool
parse_sync_method(const char *optarg, DataDirSyncMethod *sync_method)
{
	if (strcmp(optarg, "fsync") == 0)
		*sync_method = DATA_DIR_SYNC_METHOD_FSYNC;
	else if (strcmp(optarg, "syncfs") == 0)
	{
#ifdef HAVE_SYNCFS
		*sync_method = DATA_DIR_SYNC_METHOD_SYNCFS;
#else
		pg_log_error("this build does not support sync method \"%s\"", "syncfs");
		return false;
#endif
	}
	else
	{
		pg_log_error("unrecognized sync method: %s", optarg);
		return false;
	}

	return true;
}

void
appendShellString(PQExpBuffer buf, const char *str)
{
	if (!appendShellStringNoError(buf, str))
	{
		fprintf(stderr,
				"shell command argument contains a newline or carriage return: \"%s\"\n",
				str);
		exit(EXIT_FAILURE);
	}
}

bool
appendShellStringNoError(PQExpBuffer buf, const char *str)
{
	bool ok = true;
	const char *p;

	if (*str != '\0' &&
		strspn(str, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_./:") == strlen(str))
	{
		appendPQExpBufferStr(buf, str);
		return ok;
	}

	appendPQExpBufferChar(buf, '\'');
	for (p = str; *p; p++)
	{
		if (*p == '\n' || *p == '\r')
		{
			ok = false;
			continue;
		}

		if (*p == '\'')
			appendPQExpBufferStr(buf, "'\"'\"'");
		else
			appendPQExpBufferChar(buf, *p);
	}
	appendPQExpBufferChar(buf, '\'');

	return ok;
}

const char *
pg_encoding_to_char(int encoding)
{
	return pg_encoding_to_char_private(encoding);
}

int
pg_valid_server_encoding(const char *name)
{
	return pg_valid_server_encoding_private(name);
}

int
pg_valid_server_encoding_id(int encoding)
{
	return pg_valid_server_encoding_id_private(encoding);
}

const char *
select_default_timezone(const char *share_path)
{
	(void) share_path;
	return "UTC";
}
