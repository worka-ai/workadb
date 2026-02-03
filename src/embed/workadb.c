#include "postgres.h"
#include "workadb.h"

#include "libpq/workadb_io.h"
#include "tcop/workadb_embed.h"
#include "utils/elog.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <stdarg.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

typedef struct workadb_asset_entry
{
	const char *path;
	uint32_t offset;
	uint32_t size;
	uint32_t mode;
} workadb_asset_entry;

extern const unsigned char workadb_assets_blob[];
extern const unsigned char workadb_assets_blob_end[];
extern const workadb_asset_entry workadb_assets_manifest[];
extern const size_t workadb_assets_manifest_len;
extern const char workadb_assets_version[];

struct wepg_engine {
    char last_error[512];
    char *pgdata_path;
    char *temp_path;
    uint32_t flags;
    uint32_t max_response_bytes;
    struct wepg_singleton *singleton;
    bool recovery_attempted;
    bool recovery_reset_attempted;
};

static wepg_log_fn wepg_logger = NULL;
static void *wepg_logger_ctx = NULL;
static emit_log_hook_type wepg_prev_emit_log_hook = NULL;
static char workadb_assets_error[512] = {0};

typedef enum {
	WEPG_TASK_INITDB,
	WEPG_TASK_STEP,
	WEPG_TASK_RESET,
	WEPG_TASK_SHUTDOWN
} wepg_task_type;

typedef struct wepg_task {
	wepg_task_type type;
	struct wepg_engine *engine;
	uint8_t *sql_bytes;
	size_t sql_len;
	const wepg_param *params;
	size_t param_count;
	uint8_t *out_bytes;
	size_t out_cap;
	size_t out_len;
	int64_t out_rows;
	wepg_status status;
	char errbuf[512];
	bool done;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	struct wepg_task *next;
} wepg_task;

typedef struct wepg_singleton {
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	pthread_t worker;
	bool worker_running;
	bool worker_stop;
	bool shutting_down;
	wepg_task *head;
	wepg_task *tail;
	uint32_t flags;
	char *pgdata_path;
	int refcount;
} wepg_singleton;

static pthread_mutex_t wepg_singleton_mutex = PTHREAD_MUTEX_INITIALIZER;
static wepg_singleton *wepg_singleton_state = NULL;

static void *wepg_worker_main(void *arg);
static wepg_status wepg_initdb_internal(struct wepg_engine *engine, char *errbuf, size_t errlen);
static int wepg_exec_internal_with_error(struct wepg_engine *engine, const char *sql, char *errbuf, size_t errlen);
static int workadb_try_recover(struct wepg_engine *engine, char *errbuf, size_t errlen);
static int wepg_pgdata_exists(const char *pgdata_path);
static int workadb_mkdir_p(const char *path, mode_t mode);
static int workadb_ensure_pkglib(const char *exec_path, char *errbuf, size_t errlen);
static int workadb_run_resetwal(struct wepg_engine *engine, char *errbuf, size_t errlen);
static int workadb_apply_embedded_preset(const char *pgdata, char *errbuf, size_t errlen);
static int workadb_line_matches_key(const char *line, const char *key);

static void
wepg_set_error_buf(char *errbuf, size_t errlen, const char *message)
{
	if (!errbuf || errlen == 0)
		return;
	if (!message)
	{
		errbuf[0] = '\0';
		return;
	}
	strncpy(errbuf, message, errlen - 1);
	errbuf[errlen - 1] = '\0';
}

static void
workadb_set_assets_error(const char *message)
{
	if (!message)
	{
		workadb_assets_error[0] = '\0';
		return;
	}
	strncpy(workadb_assets_error, message, sizeof(workadb_assets_error) - 1);
	workadb_assets_error[sizeof(workadb_assets_error) - 1] = '\0';
}

static void
workadb_set_assets_errorf(const char *fmt, ...)
{
	va_list args;
	if (!fmt)
	{
		workadb_set_assets_error(NULL);
		return;
	}
	va_start(args, fmt);
	vsnprintf(workadb_assets_error, sizeof(workadb_assets_error), fmt, args);
	va_end(args);
	workadb_assets_error[sizeof(workadb_assets_error) - 1] = '\0';
}

static int
workadb_resolve_assets_root(char *out, size_t outlen, char *errbuf, size_t errlen)
{
	const char *base = getenv("WORKADB_HOME");
	const char *home = NULL;
	if (base && base[0] != '\0')
	{
		if (snprintf(out, outlen, "%s", base) >= (int) outlen)
		{
			wepg_set_error_buf(errbuf, errlen, "WORKADB_HOME too long");
			return 0;
		}
		return 1;
	}

	home = getenv("HOME");
	if (!home || home[0] == '\0')
	{
		wepg_set_error_buf(errbuf, errlen, "WORKADB_HOME not set and HOME not set");
		return 0;
	}
	if (snprintf(out, outlen, "%s/.workadb", home) >= (int) outlen)
	{
		wepg_set_error_buf(errbuf, errlen, "HOME path too long");
		return 0;
	}
	return 1;
}

static int
workadb_ensure_assets(struct wepg_engine *engine, char *errbuf, size_t errlen)
{
	const char *template_path = getenv("WORKADB_TEMPLATE_PATH");
	const char *exec_path = getenv("WORKADB_EXEC_PATH");
	const char *last_err = NULL;
	char base_dir[PATH_MAX];
	char assets_root[PATH_MAX];
	char template_buf[PATH_MAX];
	char exec_buf[PATH_MAX];
	(void) engine;

	if (template_path && template_path[0] != '\0'
		&& exec_path && exec_path[0] != '\0')
	{
		if (access(template_path, F_OK) != 0)
		{
			wepg_set_error_buf(errbuf, errlen, "WORKADB_TEMPLATE_PATH missing");
			return 0;
		}
		if (access(exec_path, F_OK) != 0)
		{
			wepg_set_error_buf(errbuf, errlen, "WORKADB_EXEC_PATH missing");
			return 0;
		}
		return workadb_ensure_pkglib(exec_path, errbuf, errlen);
	}

	if (!workadb_resolve_assets_root(base_dir, sizeof(base_dir), errbuf, errlen))
		return 0;

	if (snprintf(assets_root, sizeof(assets_root), "%s/assets", base_dir) >= (int) sizeof(assets_root))
	{
		wepg_set_error_buf(errbuf, errlen, "assets root path too long");
		return 0;
	}

	if (!workadb_assets_install(assets_root))
	{
		last_err = workadb_assets_last_error();
		if (last_err && last_err[0] != '\0')
			wepg_set_error_buf(errbuf, errlen, last_err);
		else
			wepg_set_error_buf(errbuf, errlen, "workadb assets install failed");
		return 0;
	}

	if (snprintf(template_buf, sizeof(template_buf), "%s/share/workadb/pgdata-template", assets_root) >= (int) sizeof(template_buf))
	{
		wepg_set_error_buf(errbuf, errlen, "template path too long");
		return 0;
	}
	if (snprintf(exec_buf, sizeof(exec_buf), "%s/bin/postgres", assets_root) >= (int) sizeof(exec_buf))
	{
		wepg_set_error_buf(errbuf, errlen, "exec path too long");
		return 0;
	}
	if (access(template_buf, F_OK) != 0)
	{
		wepg_set_error_buf(errbuf, errlen, "template path missing after install");
		return 0;
	}
	if (access(exec_buf, F_OK) != 0)
	{
		wepg_set_error_buf(errbuf, errlen, "postgres binary missing after install");
		return 0;
	}

	(void) setenv("WORKADB_TEMPLATE_PATH", template_buf, 1);
	(void) setenv("WORKADB_EXEC_PATH", exec_buf, 1);

	return workadb_ensure_pkglib(exec_buf, errbuf, errlen);
}

static void
wepg_emit_log(ErrorData *edata)
{
    if (wepg_logger && edata && edata->message)
        wepg_logger(edata->elevel, edata->message, wepg_logger_ctx);

    if (wepg_prev_emit_log_hook)
        wepg_prev_emit_log_hook(edata);
}

static int
workadb_try_recover(struct wepg_engine *engine, char *errbuf, size_t errlen)
{
    if (!engine || !engine->pgdata_path || !engine->temp_path)
        return 0;
    /* Embedded mode: never attempt to SIGTERM random PIDs, and never run
       destructive recovery actions by default. If a stale postmaster.pid is
       present (common after an unclean shutdown), remove it and let crash
       recovery proceed normally. */
	{
		char pid_path[1024];
		snprintf(pid_path, sizeof(pid_path), "%s/postmaster.pid", engine->pgdata_path);
		if (access(pid_path, F_OK) == 0)
		{
			if (unlink(pid_path) != 0)
			{
				snprintf(errbuf, errlen, "failed to remove postmaster.pid: %s", strerror(errno));
				return 0;
			}
		}
	}

	if (errbuf && errbuf[0] != '\0')
	{
		const char *allow_resetwal = getenv("WORKADB_ALLOW_RESETWAL");
		bool enabled = allow_resetwal && allow_resetwal[0] != '\0' &&
					   (strcmp(allow_resetwal, "1") == 0 ||
						strcasecmp(allow_resetwal, "true") == 0);

		if (enabled &&
			(strstr(errbuf, "could not locate a valid checkpoint record") ||
			 strstr(errbuf, "invalid checkpoint record") ||
			 strstr(errbuf, "unexpected zero page") ||
			 strstr(errbuf, "recovery timeout")) &&
			!engine->recovery_reset_attempted)
		{
			engine->recovery_reset_attempted = true;
			if (workadb_run_resetwal(engine, errbuf, errlen))
			{
				errbuf[0] = '\0';
				return 1;
			}
			return 0;
		}
	}

	errbuf[0] = '\0';
	return 1;
}

static void
wepg_set_error(struct wepg_engine *engine, const char *message) {
    if (!engine) {
        return;
    }
    if (!message) {
        engine->last_error[0] = '\0';
        return;
    }
    strncpy(engine->last_error, message, sizeof(engine->last_error) - 1);
    engine->last_error[sizeof(engine->last_error) - 1] = '\0';
}

static void
wepg_log_message(int level, const char *message)
{
	if (!message)
		return;
	if (wepg_logger)
		wepg_logger(level, message, wepg_logger_ctx);
}

static void
wepg_log_sql_preview(const uint8_t *sql_bytes, size_t sql_len, size_t param_count)
{
	char preview[256];
	size_t copy_len = 0;
	size_t i;

	if (!sql_bytes || sql_len == 0)
	{
		wepg_log_message(15, "workadb exec_sql len=0");
		return;
	}

	copy_len = sql_len;
	if (copy_len > sizeof(preview) - 1)
		copy_len = sizeof(preview) - 1;

	for (i = 0; i < copy_len; i++)
	{
		unsigned char c = sql_bytes[i];
		preview[i] = (c >= 32 && c < 127) ? (char) c : '?';
	}
	preview[copy_len] = '\0';

	{
		char msg[512];
		snprintf(msg, sizeof(msg),
				 "workadb exec_sql len=%zu params=%zu preview=\"%s\"",
				 sql_len, param_count, preview);
		wepg_log_message(15, msg);
	}
}

static char *
wepg_default_pgdata_path(void)
{
	const char *override = getenv("WORKADB_DATA_DIR");
	const char *tmpdir = getenv("TMPDIR");
	char path[PATH_MAX];

	if (override && override[0])
		return strdup(override);

	if (access("/dev/shm", F_OK) == 0)
	{
		if (snprintf(path, sizeof(path), "/dev/shm/workadb") < (int) sizeof(path))
			return strdup(path);
	}

	if (tmpdir && tmpdir[0])
	{
		if (snprintf(path, sizeof(path), "%s/workadb", tmpdir) < (int) sizeof(path))
			return strdup(path);
	}

	if (!getcwd(path, sizeof(path)))
	{
		if (snprintf(path, sizeof(path), "/tmp/workadb") >= (int) sizeof(path))
			return NULL;
		return strdup(path);
	}

	if (snprintf(path, sizeof(path), "%s/workadb", path) >= (int) sizeof(path))
		return NULL;

	return strdup(path);
}

static char *
wepg_normalize_path(const char *path)
{
	char cwd[PATH_MAX];
	char absbuf[PATH_MAX];
	char candidate[PATH_MAX];
	char *resolved_prefix = NULL;
	char *result = NULL;
	size_t len;
	enum
	{
		WEPG_MAX_PATH_PARTS = (PATH_MAX / 2) + 1
	};
	char	   *parts[WEPG_MAX_PATH_PARTS];
	int			nparts = 0;
	int			i;

	if (!path)
		return NULL;

	/* Build an absolute path first. */
	if (path[0] == '/')
	{
		if (snprintf(absbuf, sizeof(absbuf), "%s", path) >= (int) sizeof(absbuf))
			return NULL;
	}
	else
	{
		if (!getcwd(cwd, sizeof(cwd)))
			return strdup(path);
		if (snprintf(absbuf, sizeof(absbuf), "%s/%s", cwd, path) >= (int) sizeof(absbuf))
			return strdup(path);
	}

	/* Strip trailing slashes (except for root). */
	len = strlen(absbuf);
	while (len > 1 && absbuf[len - 1] == '/')
	{
		absbuf[len - 1] = '\0';
		len--;
	}

	/*
	 * realpath() fails if the full path doesn't exist. To keep normalization
	 * stable across "init if missing" flows, resolve the longest existing
	 * prefix and append the remaining suffix components.
	 */
	resolved_prefix = realpath(absbuf, NULL);
	if (resolved_prefix)
		return resolved_prefix;

	if (snprintf(candidate, sizeof(candidate), "%s", absbuf) >= (int) sizeof(candidate))
		return strdup(absbuf);

	memset(parts, 0, sizeof(parts));
	while (!resolved_prefix)
	{
		char *slash;
		const char *part;

		resolved_prefix = realpath(candidate, NULL);
		if (resolved_prefix)
			break;

		if (!(errno == ENOENT || errno == ENOTDIR))
			break;

		slash = strrchr(candidate, '/');
		if (!slash)
			break;

		part = slash + 1;
		if (*part == '\0')
			break;
		if (nparts >= (int) WEPG_MAX_PATH_PARTS)
			break;

		parts[nparts++] = strdup(part);
		*slash = '\0';
		if (candidate[0] == '\0')
			snprintf(candidate, sizeof(candidate), "%s", "/");
	}

	if (!resolved_prefix)
	{
		for (i = 0; i < nparts; i++)
			free(parts[i]);
		return strdup(absbuf);
	}

	{
		size_t out_len = strlen(resolved_prefix);
		for (i = nparts - 1; i >= 0; i--)
		{
			out_len += 1 + strlen(parts[i]);
			if (i == 0)
				break;
		}

		result = (char *) malloc(out_len + 1);
		if (!result)
		{
			for (i = 0; i < nparts; i++)
				free(parts[i]);
			free(resolved_prefix);
			return strdup(absbuf);
		}

		strcpy(result, resolved_prefix);
		free(resolved_prefix);
		resolved_prefix = NULL;

		for (i = nparts - 1; i >= 0; i--)
		{
			size_t cur = strlen(result);
			if (cur == 0 || result[cur - 1] != '/')
				strcat(result, "/");
			strcat(result, parts[i]);
			free(parts[i]);
			if (i == 0)
				break;
		}
	}

	return result;
}

static wepg_status
wepg_singleton_acquire(const char *pgdata_path, uint32_t flags, wepg_singleton **out, char *errbuf, size_t errlen)
{
	const uint32_t mask = WEPG_FLAG_ENABLE_WIRE | WEPG_FLAG_READONLY | WEPG_FLAG_DISABLE_DYNAMIC_EXT;
	char *normalized = NULL;
	wepg_singleton *singleton = NULL;

	if (!pgdata_path || pgdata_path[0] == '\0')
	{
		wepg_set_error_buf(errbuf, errlen, "pgdata path missing");
		return WEPG_ERR_INVALID_CONFIG;
	}

	normalized = wepg_normalize_path(pgdata_path);
	if (!normalized)
	{
		wepg_set_error_buf(errbuf, errlen, "failed to normalize pgdata path");
		return WEPG_ERR_INTERNAL;
	}

	pthread_mutex_lock(&wepg_singleton_mutex);
	singleton = wepg_singleton_state;
	if (!singleton)
	{
		singleton = (wepg_singleton *) calloc(1, sizeof(wepg_singleton));
		if (!singleton)
		{
			pthread_mutex_unlock(&wepg_singleton_mutex);
			free(normalized);
			wepg_set_error_buf(errbuf, errlen, "failed to allocate singleton");
			return WEPG_ERR_INTERNAL;
		}
		pthread_mutex_init(&singleton->mutex, NULL);
		pthread_cond_init(&singleton->cond, NULL);
		singleton->flags = flags & mask;
		singleton->pgdata_path = normalized;
		singleton->refcount = 1;
		singleton->worker_running = true;
		wepg_singleton_state = singleton;
		if (pthread_create(&singleton->worker, NULL, wepg_worker_main, singleton) != 0)
		{
			singleton->worker_running = false;
			wepg_singleton_state = NULL;
			pthread_mutex_unlock(&wepg_singleton_mutex);
			pthread_cond_destroy(&singleton->cond);
			pthread_mutex_destroy(&singleton->mutex);
			free(singleton->pgdata_path);
			free(singleton);
			wepg_set_error_buf(errbuf, errlen, "failed to start worker thread");
			return WEPG_ERR_INTERNAL;
		}
		pthread_mutex_unlock(&wepg_singleton_mutex);
		*out = singleton;
		return WEPG_OK;
	}

	if (singleton->shutting_down)
	{
		pthread_mutex_unlock(&wepg_singleton_mutex);
		free(normalized);
		wepg_set_error_buf(errbuf, errlen, "workadb shutting down");
		return WEPG_ERR_INTERNAL;
	}

		if (strcmp(singleton->pgdata_path, normalized) != 0)
		{
			snprintf(errbuf, errlen,
					 "workadb already initialized with different pgdata path: existing=\"%s\" new=\"%s\"",
					 singleton->pgdata_path ? singleton->pgdata_path : "(null)",
					 normalized ? normalized : "(null)");
			pthread_mutex_unlock(&wepg_singleton_mutex);
			free(normalized);
			return WEPG_ERR_INCOMPATIBLE_PGDATA;
		}
	if ((flags & mask) != singleton->flags)
	{
		pthread_mutex_unlock(&wepg_singleton_mutex);
		free(normalized);
		wepg_set_error_buf(errbuf, errlen, "workadb already initialized with different flags");
		return WEPG_ERR_INVALID_CONFIG;
	}

	singleton->refcount++;
	pthread_mutex_unlock(&wepg_singleton_mutex);
	free(normalized);
	*out = singleton;
	return WEPG_OK;
}

static bool
wepg_singleton_release(wepg_singleton *singleton)
{
	bool is_last = false;

	if (!singleton)
		return false;

	pthread_mutex_lock(&wepg_singleton_mutex);
	if (singleton->refcount > 0)
		singleton->refcount--;
	if (singleton->refcount == 0)
		is_last = true;
	pthread_mutex_unlock(&wepg_singleton_mutex);

	return is_last;
}

static void
wepg_singleton_destroy(wepg_singleton *singleton)
{
	if (!singleton)
		return;

	pthread_join(singleton->worker, NULL);
	pthread_mutex_lock(&wepg_singleton_mutex);
	if (wepg_singleton_state == singleton)
		wepg_singleton_state = NULL;
	pthread_mutex_unlock(&wepg_singleton_mutex);

	pthread_cond_destroy(&singleton->cond);
	pthread_mutex_destroy(&singleton->mutex);
	free(singleton->pgdata_path);
	free(singleton);
}

static void
wepg_singleton_stop(wepg_singleton *singleton)
{
	if (!singleton)
		return;

	pthread_mutex_lock(&wepg_singleton_mutex);
	singleton->shutting_down = true;
	pthread_mutex_unlock(&wepg_singleton_mutex);

	pthread_mutex_lock(&singleton->mutex);
	singleton->worker_stop = true;
	pthread_cond_signal(&singleton->cond);
	pthread_mutex_unlock(&singleton->mutex);

	wepg_singleton_destroy(singleton);
}

static void
wepg_task_init(wepg_task *task)
{
	pthread_mutex_init(&task->mutex, NULL);
	pthread_cond_init(&task->cond, NULL);
	task->done = false;
	task->next = NULL;
	task->status = WEPG_OK;
	task->errbuf[0] = '\0';
}

static void
wepg_task_complete(wepg_task *task)
{
	pthread_mutex_lock(&task->mutex);
	task->done = true;
	pthread_cond_signal(&task->cond);
	pthread_mutex_unlock(&task->mutex);
}

static void
wepg_queue_task(wepg_singleton *singleton, wepg_task *task)
{
	pthread_mutex_lock(&singleton->mutex);
	if (singleton->tail)
		singleton->tail->next = task;
	else
		singleton->head = task;
	singleton->tail = task;
	pthread_cond_signal(&singleton->cond);
	pthread_mutex_unlock(&singleton->mutex);
}

static void
wepg_wait_task(wepg_task *task)
{
	pthread_mutex_lock(&task->mutex);
	while (!task->done)
		pthread_cond_wait(&task->cond, &task->mutex);
	pthread_mutex_unlock(&task->mutex);
	pthread_cond_destroy(&task->cond);
	pthread_mutex_destroy(&task->mutex);
}

static void *
wepg_worker_main(void *arg)
{
	wepg_singleton *singleton = (wepg_singleton *) arg;

	for (;;)
	{
		wepg_task *task = NULL;

		pthread_mutex_lock(&singleton->mutex);
		while (!singleton->head && !singleton->worker_stop)
			pthread_cond_wait(&singleton->cond, &singleton->mutex);
		if (singleton->worker_stop && !singleton->head)
		{
			pthread_mutex_unlock(&singleton->mutex);
			break;
		}
		task = singleton->head;
		if (task)
		{
			singleton->head = task->next;
			if (!singleton->head)
				singleton->tail = NULL;
		}
		pthread_mutex_unlock(&singleton->mutex);

		if (!task)
			continue;

		switch (task->type)
		{
			case WEPG_TASK_INITDB:
				wepg_log_message(15, "workadb initdb start");
				task->status = wepg_initdb_internal(task->engine, task->errbuf, sizeof(task->errbuf));
				if (task->status != WEPG_OK)
				{
					char msg[640];
					snprintf(msg, sizeof(msg), "workadb initdb failed: %s", task->errbuf);
					wepg_log_message(15, msg);
				}
				break;
			case WEPG_TASK_STEP:
			{
				const char *db_name = "worka";
				const char *user_name = "worka";
				bool backend_started = false;
				wepg_log_sql_preview(task->sql_bytes, task->sql_len, task->param_count);
				if (!task->engine->recovery_attempted
					&& wepg_pgdata_exists(task->engine->pgdata_path))
				{
					task->engine->recovery_attempted = true;
					wepg_log_message(15, "workadb preflight recovery check");
					if (!workadb_try_recover(task->engine, task->errbuf, sizeof(task->errbuf)))
					{
						task->status = WEPG_ERR_EXEC_FAILED;
						wepg_log_message(15, task->errbuf);
						break;
					}
				}
				backend_started = workadb_backend_start(task->engine->pgdata_path, db_name,
														user_name, task->errbuf,
														sizeof(task->errbuf));
				if (!backend_started && !task->engine->recovery_attempted)
				{
					task->engine->recovery_attempted = true;
					wepg_log_message(15, "workadb backend start failed, attempting recovery");
					if (workadb_try_recover(task->engine, task->errbuf, sizeof(task->errbuf)))
					{
						backend_started = workadb_backend_start(task->engine->pgdata_path, db_name,
																user_name, task->errbuf,
																sizeof(task->errbuf));
					}
				}
				if (!backend_started)
				{
					task->status = WEPG_ERR_EXEC_FAILED;
					wepg_log_message(15, task->errbuf);
					break;
				}
				if (task->param_count > 0)
				{
					if (!workadb_backend_exec_sql_params(task->sql_bytes,
														task->sql_len,
														task->params,
														task->param_count,
														task->out_bytes,
														task->out_cap,
														&task->out_len,
														&task->out_rows,
														task->errbuf,
														sizeof(task->errbuf)))
					{
						task->status = WEPG_ERR_EXEC_FAILED;
						wepg_log_message(15, task->errbuf);
						break;
					}
				}
				else if (!workadb_backend_exec_sql(task->sql_bytes,
													task->sql_len,
													task->out_bytes,
													task->out_cap,
													&task->out_len,
													&task->out_rows,
													task->errbuf,
													sizeof(task->errbuf)))
				{
					task->status = WEPG_ERR_EXEC_FAILED;
					wepg_log_message(15, task->errbuf);
					break;
				}
				task->status = WEPG_OK;
				break;
			}
			case WEPG_TASK_RESET:
				if (workadb_backend_is_started())
				{
					if (!wepg_exec_internal_with_error(task->engine, "DISCARD ALL", task->errbuf, sizeof(task->errbuf)))
					{
						task->status = WEPG_ERR_EXEC_FAILED;
						break;
					}
				}
				workadb_io_reset();
				task->status = WEPG_OK;
				break;
			case WEPG_TASK_SHUTDOWN:
				workadb_io_reset();
				workadb_backend_shutdown();
				task->status = WEPG_OK;
				break;
		}

		wepg_task_complete(task);
	}

	return NULL;
}

uint32_t
wepg_abi_version(void) {
    return ((uint32_t) WEPG_ABI_MAJOR << 16) | (uint32_t) WEPG_ABI_MINOR;
}

const char *
wepg_version_string(void) {
	    return "workadb/0.1 pg=" PG_VERSION;
}

void
wepg_set_logger(wepg_log_fn fn, void *ctx) {
    wepg_logger = fn;
    wepg_logger_ctx = ctx;
    if (wepg_logger) {
        wepg_prev_emit_log_hook = emit_log_hook;
        emit_log_hook = wepg_emit_log;
        wepg_logger(1, "workadb logger installed", wepg_logger_ctx);
    } else if (emit_log_hook == wepg_emit_log) {
        emit_log_hook = wepg_prev_emit_log_hook;
        wepg_prev_emit_log_hook = NULL;
    }
}

wepg_status
wepg_init(const wepg_config *config, wepg_handle *out) {
    struct wepg_engine *engine = NULL;
    wepg_status status = WEPG_OK;
	char errbuf[512] = {0};
	char *default_path = NULL;

    if (!config || !out) {
        return WEPG_ERR_INVALID_CONFIG;
    }
    /* Ensure embedded mode is visible to backend startup checks. */
    setenv("WORKADB_EMBEDDED", "1", 1);

    engine = (struct wepg_engine *) calloc(1, sizeof(struct wepg_engine));
    if (!engine) {
        return WEPG_ERR_INTERNAL;
    }

    if (!config->pgdata_path || config->pgdata_path[0] == '\0')
	{
		default_path = wepg_default_pgdata_path();
		if (!default_path)
		{
			free(engine);
			return WEPG_ERR_INVALID_CONFIG;
		}
		engine->pgdata_path = strdup(default_path);
		free(default_path);
	}
	else
	{
		engine->pgdata_path = strdup(config->pgdata_path);
	}
    if (!engine->pgdata_path) {
        free(engine);
        return WEPG_ERR_INTERNAL;
    }

    if (config->temp_path && config->temp_path[0] != '\0') {
        engine->temp_path = strdup(config->temp_path);
    } else {
        engine->temp_path = strdup(engine->pgdata_path);
    }
    if (!engine->temp_path) {
        free(engine->pgdata_path);
        free(engine);
        return WEPG_ERR_INTERNAL;
    }

    engine->flags = config->flags;
    engine->max_response_bytes = config->max_response_bytes;
    engine->last_error[0] = '\0';

	status = wepg_singleton_acquire(engine->pgdata_path, engine->flags, &engine->singleton, errbuf, sizeof(errbuf));
	if (status != WEPG_OK)
	{
		wepg_set_error(engine, errbuf);
		free(engine->pgdata_path);
		free(engine->temp_path);
		free(engine);
		return status;
	}
	if (engine->singleton && engine->singleton->pgdata_path)
	{
		char *normalized = strdup(engine->singleton->pgdata_path);
		if (!normalized)
		{
			wepg_set_error(engine, "failed to allocate pgdata path");
			if (wepg_singleton_release(engine->singleton))
				wepg_singleton_stop(engine->singleton);
			free(engine->pgdata_path);
			free(engine->temp_path);
			free(engine);
			return WEPG_ERR_INTERNAL;
		}
		free(engine->pgdata_path);
		engine->pgdata_path = normalized;
	}

    *out = engine;

    if ((engine->flags & WEPG_FLAG_NO_AUTO_INITDB) == 0) {
        status = wepg_initdb(engine);
        if (status != WEPG_OK) {
			if (wepg_singleton_release(engine->singleton))
				wepg_singleton_stop(engine->singleton);
            free(engine->pgdata_path);
            free(engine->temp_path);
            free(engine);
            *out = NULL;
            return status;
        }
    }

    return WEPG_OK;
}

static int
wepg_pgdata_exists(const char *pgdata_path)
{
	char path[1024];

	if (!pgdata_path || pgdata_path[0] == '\0')
		return 0;
	snprintf(path, sizeof(path), "%s/PG_VERSION", pgdata_path);
	return access(path, F_OK) == 0;
}

static int
wepg_exec_internal_with_error(struct wepg_engine *engine, const char *sql, char *errbuf, size_t errlen)
{
	uint8_t scratch[8192];

	if (!engine || !sql)
		return 0;

	workadb_io_set_output(scratch, sizeof(scratch));
	workadb_io_set_input(NULL, 0);
	workadb_io_enable(true);

	if (!workadb_backend_exec(sql, errbuf, errlen))
	{
		workadb_io_enable(false);
		return 0;
	}

	workadb_io_enable(false);
	return 1;
}

static int
wepg_write_pwfile(const char *dir_path, char *out_path, size_t out_len)
{
	char template_path[1024];
	const char *base = dir_path && dir_path[0] ? dir_path : "/tmp";
	int fd = -1;
	FILE *fp = NULL;

	if (!out_path || out_len == 0)
		return 0;

	snprintf(template_path, sizeof(template_path), "%s/workadb_pwXXXXXX", base);
	fd = mkstemp(template_path);
	if (fd < 0)
		return 0;

	fp = fdopen(fd, "w");
	if (!fp)
	{
		close(fd);
		unlink(template_path);
		return 0;
	}

	if (fprintf(fp, "worka\n") <= 0)
	{
		fclose(fp);
		unlink(template_path);
		return 0;
	}

	fclose(fp);
	snprintf(out_path, out_len, "%s", template_path);
	return 1;
}

static size_t
workadb_parse_octal(const char *value, size_t len)
{
	size_t result = 0;
	size_t i = 0;

	if (!value)
		return 0;

	while (i < len && (value[i] == ' ' || value[i] == '\0'))
		i++;

	for (; i < len && value[i] >= '0' && value[i] <= '7'; i++)
		result = (result << 3) + (size_t) (value[i] - '0');

	return result;
}

static int
workadb_is_safe_path(const char *path)
{
	const char *p = path;
	const char *segment = path;

	if (!path || path[0] == '\0')
		return 0;
	if (path[0] == '/')
		return 0;

	while (*p)
	{
		if (*p == '/')
		{
			size_t len = (size_t) (p - segment);
			if (len == 0)
				return 0;
			if (len == 2 && segment[0] == '.' && segment[1] == '.')
				return 0;
			segment = p + 1;
		}
		p++;
	}
	if (segment && *segment)
	{
		size_t len = (size_t) (p - segment);
		if (len == 2 && segment[0] == '.' && segment[1] == '.')
			return 0;
	}
	return 1;
}

static int
workadb_mkdir_p(const char *path, mode_t mode)
{
	char tmp[PATH_MAX];
	size_t len;
	size_t i;

	if (!path)
		return 0;

	len = strnlen(path, sizeof(tmp));
	if (len == 0 || len >= sizeof(tmp))
		return 0;

	memcpy(tmp, path, len + 1);
	for (i = 1; i < len; i++)
	{
		if (tmp[i] == '/')
		{
			tmp[i] = '\0';
			if (tmp[0] != '\0' && mkdir(tmp, mode) != 0 && errno != EEXIST)
				return 0;
			tmp[i] = '/';
		}
	}
	if (mkdir(tmp, mode) != 0 && errno != EEXIST)
		return 0;
	return 1;
}

static int
workadb_ensure_pkglib(const char *exec_path, char *errbuf, size_t errlen)
{
	char root[PATH_MAX];
	char pkglib[PATH_MAX];
	char *last = NULL;
	char *prev = NULL;

	if (!exec_path || exec_path[0] == '\0')
	{
		wepg_set_error_buf(errbuf, errlen, "WORKADB_EXEC_PATH missing");
		return 0;
	}
	if (strnlen(exec_path, sizeof(root)) >= sizeof(root))
	{
		wepg_set_error_buf(errbuf, errlen, "exec path too long");
		return 0;
	}

	strncpy(root, exec_path, sizeof(root) - 1);
	root[sizeof(root) - 1] = '\0';

	last = strrchr(root, '/');
	if (!last)
	{
		wepg_set_error_buf(errbuf, errlen, "invalid exec path");
		return 0;
	}
	*last = '\0';
	prev = strrchr(root, '/');
	if (!prev)
	{
		wepg_set_error_buf(errbuf, errlen, "invalid exec path root");
		return 0;
	}
	*prev = '\0';

	if (snprintf(pkglib, sizeof(pkglib), "%s/lib/postgresql", root) >= (int) sizeof(pkglib))
	{
		wepg_set_error_buf(errbuf, errlen, "pkglib path too long");
		return 0;
	}
	if (!workadb_mkdir_p(pkglib, 0755))
	{
		wepg_set_error_buf(errbuf, errlen, "failed to create pkglib directory");
		return 0;
	}

	return 1;
}

static int
workadb_run_resetwal(struct wepg_engine *engine, char *errbuf, size_t errlen)
{
	const char *exec_path = getenv("WORKADB_EXEC_PATH");
	char root[PATH_MAX];
	char resetwal[PATH_MAX];
	char *last = NULL;
	char *prev = NULL;
	char *argv[6];
	posix_spawn_file_actions_t actions;
	pid_t pid = -1;
	int status = 0;

	if (!engine || !engine->pgdata_path)
		return 0;
	if (!exec_path || exec_path[0] == '\0')
	{
		wepg_set_error_buf(errbuf, errlen, "WORKADB_EXEC_PATH missing");
		return 0;
	}
	if (strnlen(exec_path, sizeof(root)) >= sizeof(root))
	{
		wepg_set_error_buf(errbuf, errlen, "exec path too long");
		return 0;
	}

	strncpy(root, exec_path, sizeof(root) - 1);
	root[sizeof(root) - 1] = '\0';
	last = strrchr(root, '/');
	if (!last)
	{
		wepg_set_error_buf(errbuf, errlen, "invalid exec path");
		return 0;
	}
	*last = '\0';
	prev = strrchr(root, '/');
	if (!prev)
	{
		wepg_set_error_buf(errbuf, errlen, "invalid exec path root");
		return 0;
	}
	*prev = '\0';

	if (snprintf(resetwal, sizeof(resetwal), "%s/bin/pg_resetwal", root) >= (int) sizeof(resetwal))
	{
		wepg_set_error_buf(errbuf, errlen, "resetwal path too long");
		return 0;
	}
	if (access(resetwal, X_OK) != 0)
	{
		wepg_set_error_buf(errbuf, errlen, "pg_resetwal missing");
		return 0;
	}

	argv[0] = (char *) resetwal;
	argv[1] = "-f";
	argv[2] = "-D";
	argv[3] = engine->pgdata_path;
	argv[4] = NULL;

	posix_spawn_file_actions_init(&actions);
	if (posix_spawn(&pid, resetwal, &actions, NULL, argv, environ) != 0)
	{
		posix_spawn_file_actions_destroy(&actions);
		wepg_set_error_buf(errbuf, errlen, "pg_resetwal spawn failed");
		return 0;
	}
	posix_spawn_file_actions_destroy(&actions);
	if (waitpid(pid, &status, 0) < 0)
	{
		wepg_set_error_buf(errbuf, errlen, "pg_resetwal wait failed");
		return 0;
	}
	if (status != 0)
	{
		wepg_set_error_buf(errbuf, errlen, "pg_resetwal failed");
		return 0;
	}

	errbuf[0] = '\0';
	return 1;
}

wepg_status
workadb_assets_install(const char *dir_path)
{
	size_t workadb_assets_blob_len = (size_t)(workadb_assets_blob_end - workadb_assets_blob);
	char version_path[PATH_MAX];
	FILE *vf = NULL;
	char existing_version[128];
	size_t version_len = 0;
	size_t i;

	if (!dir_path || dir_path[0] == '\0')
	{
		workadb_set_assets_error("invalid dest path");
		return WEPG_ERR_INVALID_CONFIG;
	}

	workadb_set_assets_error(NULL);

	if (!workadb_mkdir_p(dir_path, 0755))
	{
		workadb_set_assets_errorf("mkdir failed: %s (%s)", dir_path, strerror(errno));
		return WEPG_ERR_INTERNAL;
	}

	if (snprintf(version_path, sizeof(version_path), "%s/.workadb_assets_version", dir_path) < (int) sizeof(version_path))
	{
		vf = fopen(version_path, "rb");
		if (vf)
		{
			version_len = fread(existing_version, 1, sizeof(existing_version) - 1, vf);
			fclose(vf);
			existing_version[version_len] = '\0';
			if (strncmp(existing_version, workadb_assets_version, sizeof(existing_version) - 1) == 0)
				return WEPG_OK;
		}
	}

	for (i = 0; i < workadb_assets_manifest_len; i++)
	{
		const workadb_asset_entry *entry = &workadb_assets_manifest[i];
		const char *rel = entry->path;
		char path[PATH_MAX];
		char dirbuf[PATH_MAX];
		FILE *fp;
		const unsigned char *src;
		size_t offset;
		size_t size;
		mode_t mode;
		char *slash;

		if (!rel || rel[0] == '\0')
			continue;
		if (!workadb_is_safe_path(rel))
		{
			workadb_set_assets_errorf("unsafe path: %s", rel);
			return WEPG_ERR_INTERNAL;
		}
		if (snprintf(path, sizeof(path), "%s/%s", dir_path, rel) >= (int) sizeof(path))
		{
			workadb_set_assets_errorf("path too long: %s", rel);
			return WEPG_ERR_INTERNAL;
		}

		slash = strrchr(path, '/');
		if (slash)
		{
			size_t dlen = (size_t) (slash - path);
			if (dlen >= sizeof(dirbuf))
			{
				workadb_set_assets_errorf("path too long: %s", path);
				return WEPG_ERR_INTERNAL;
			}
			memcpy(dirbuf, path, dlen);
			dirbuf[dlen] = '\0';
			if (!workadb_mkdir_p(dirbuf, 0755))
			{
				workadb_set_assets_errorf("mkdir failed: %s (%s)", dirbuf, strerror(errno));
				return WEPG_ERR_INTERNAL;
			}
		}

		fp = fopen(path, "wb");
		if (!fp)
		{
			workadb_set_assets_errorf("open failed: %s (%s)", path, strerror(errno));
			return WEPG_ERR_INTERNAL;
		}

		offset = entry->offset;
		size = entry->size;
		if (offset + size > workadb_assets_blob_len)
		{
			fclose(fp);
			workadb_set_assets_errorf("asset out of range: %s", rel);
			return WEPG_ERR_INTERNAL;
		}
		src = workadb_assets_blob + offset;
		if (size > 0 && fwrite(src, 1, size, fp) != size)
		{
			fclose(fp);
			workadb_set_assets_errorf("write failed: %s", path);
			return WEPG_ERR_INTERNAL;
		}
		fclose(fp);

		mode = (mode_t) entry->mode;
		if (mode == 0)
			mode = (strncmp(rel, "bin/", 4) == 0) ? 0755 : 0644;
		(void) chmod(path, mode);
	}

	if (snprintf(version_path, sizeof(version_path), "%s/.workadb_assets_version", dir_path) < (int) sizeof(version_path))
	{
		vf = fopen(version_path, "wb");
		if (vf)
		{
			fwrite(workadb_assets_version, 1, strlen(workadb_assets_version), vf);
			fclose(vf);
		}
	}

	return WEPG_OK;
}

const char *
workadb_assets_last_error(void)
{
	return workadb_assets_error;
}

const char *
workadb_assets_version_string(void)
{
	return workadb_assets_version;
}

static int
workadb_copy_file(const char *src, const char *dst, char *errbuf, size_t errlen)
{
	int in_fd = -1;
	int out_fd = -1;
	char buf[64 * 1024];
	ssize_t read_bytes;
	struct stat st;

	if (stat(src, &st) != 0)
	{
		snprintf(errbuf, errlen, "stat failed for %s: %s", src, strerror(errno));
		return 0;
	}

	in_fd = open(src, O_RDONLY);
	if (in_fd < 0)
	{
		snprintf(errbuf, errlen, "open failed for %s: %s", src, strerror(errno));
		return 0;
	}

	out_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (out_fd < 0)
	{
		snprintf(errbuf, errlen, "open failed for %s: %s", dst, strerror(errno));
		close(in_fd);
		return 0;
	}

	while ((read_bytes = read(in_fd, buf, sizeof(buf))) > 0)
	{
		ssize_t offset = 0;
		while (offset < read_bytes)
		{
			ssize_t written = write(out_fd, buf + offset, (size_t) (read_bytes - offset));
			if (written < 0)
			{
				snprintf(errbuf, errlen, "write failed for %s: %s", dst, strerror(errno));
				close(in_fd);
				close(out_fd);
				return 0;
			}
			offset += written;
		}
	}

	if (read_bytes < 0)
	{
		snprintf(errbuf, errlen, "read failed for %s: %s", src, strerror(errno));
		close(in_fd);
		close(out_fd);
		return 0;
	}

	close(in_fd);
	if (fsync(out_fd) != 0)
	{
		snprintf(errbuf, errlen, "fsync failed for %s: %s", dst, strerror(errno));
		close(out_fd);
		return 0;
	}
	close(out_fd);
	return 1;
}

static int
workadb_copy_dir(const char *src, const char *dst, char *errbuf, size_t errlen)
{
	DIR *dir;
	struct dirent *entry;
	struct stat st;

	if (stat(src, &st) != 0)
	{
		snprintf(errbuf, errlen, "stat failed for %s: %s", src, strerror(errno));
		return 0;
	}

	if (mkdir(dst, 0700) != 0 && errno != EEXIST)
	{
		snprintf(errbuf, errlen, "mkdir failed for %s: %s", dst, strerror(errno));
		return 0;
	}

	dir = opendir(src);
	if (!dir)
	{
		snprintf(errbuf, errlen, "opendir failed for %s: %s", src, strerror(errno));
		return 0;
	}

	while ((entry = readdir(dir)) != NULL)
	{
		char src_path[2048];
		char dst_path[2048];

		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;

		snprintf(src_path, sizeof(src_path), "%s/%s", src, entry->d_name);
		snprintf(dst_path, sizeof(dst_path), "%s/%s", dst, entry->d_name);

		if (lstat(src_path, &st) != 0)
		{
			snprintf(errbuf, errlen, "lstat failed for %s: %s", src_path, strerror(errno));
			closedir(dir);
			return 0;
		}

		if (S_ISDIR(st.st_mode))
		{
			if (!workadb_copy_dir(src_path, dst_path, errbuf, errlen))
			{
				closedir(dir);
				return 0;
			}
		}
		else if (S_ISLNK(st.st_mode))
		{
			char link_target[2048];
			ssize_t len = readlink(src_path, link_target, sizeof(link_target) - 1);
			if (len < 0)
			{
				snprintf(errbuf, errlen, "readlink failed for %s: %s", src_path, strerror(errno));
				closedir(dir);
				return 0;
			}
			link_target[len] = '\0';
			if (symlink(link_target, dst_path) != 0)
			{
				snprintf(errbuf, errlen, "symlink failed for %s: %s", dst_path, strerror(errno));
				closedir(dir);
				return 0;
			}
		}
		else if (S_ISREG(st.st_mode))
		{
			if (!workadb_copy_file(src_path, dst_path, errbuf, errlen))
			{
				closedir(dir);
				return 0;
			}
		}
	}

	closedir(dir);
	return 1;
}

static int
workadb_line_matches_key(const char *line, const char *key)
{
	const char *p = line;
	size_t keylen = strlen(key);

	while (*p == ' ' || *p == '\t')
		p++;
	if (*p == '#' || *p == ';')
	{
		p++;
		while (*p == ' ' || *p == '\t')
			p++;
	}
	if (strncmp(p, key, keylen) != 0)
		return 0;
	if (p[keylen] == '\0' || p[keylen] == ' ' || p[keylen] == '\t' || p[keylen] == '=')
		return 1;
	return 0;
}

static int
workadb_apply_embedded_preset(const char *pgdata, char *errbuf, size_t errlen)
{
	typedef struct workadb_guc_entry
	{
		const char *key;
		const char *value;
	} workadb_guc_entry;

#if defined(__ANDROID__)
	static const workadb_guc_entry preset[] = {
		{"listen_addresses", "''"},
		{"unix_socket_directories", "'.sockets'"},
		{"shared_preload_libraries", "''"},
		{"max_connections", "3"},
		{"max_worker_processes", "0"},
		{"max_parallel_workers", "0"},
		{"max_parallel_maintenance_workers", "0"},
		{"max_parallel_workers_per_gather", "0"},
		{"max_replication_slots", "0"},
		{"max_logical_replication_workers", "0"},
		{"max_sync_workers_per_subscription", "0"},
		{"shared_buffers", "'24MB'"},
		{"work_mem", "'2MB'"},
		{"maintenance_work_mem", "'8MB'"},
		{"effective_io_concurrency", "0"},
		{"jit", "off"},
		{"wal_level", "minimal"},
		{"max_wal_senders", "0"},
		{"fsync", "on"},
		{"full_page_writes", "on"},
		{"synchronous_commit", "on"},
		{"checkpoint_completion_target", "0.9"},
		{"checkpoint_timeout", "'3min'"},
		{"max_wal_size", "'512MB'"},
		{"min_wal_size", "'64MB'"},
		{"autovacuum", "off"},
		{"bgwriter_lru_maxpages", "0"},
		{"log_min_messages", "warning"},
		{"log_checkpoints", "on"},
		{"temp_buffers", "'4MB'"},
	};
#elif defined(__APPLE__) && defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
	static const workadb_guc_entry preset[] = {
		{"listen_addresses", "''"},
		{"unix_socket_directories", "'.sockets'"},
		{"shared_preload_libraries", "''"},
		{"max_connections", "3"},
		{"max_worker_processes", "0"},
		{"max_parallel_workers", "0"},
		{"max_parallel_maintenance_workers", "0"},
		{"max_parallel_workers_per_gather", "0"},
		{"max_replication_slots", "0"},
		{"max_logical_replication_workers", "0"},
		{"max_sync_workers_per_subscription", "0"},
		{"shared_buffers", "'24MB'"},
		{"work_mem", "'2MB'"},
		{"maintenance_work_mem", "'8MB'"},
		{"effective_io_concurrency", "0"},
		{"jit", "off"},
		{"wal_level", "minimal"},
		{"max_wal_senders", "0"},
		{"fsync", "on"},
		{"full_page_writes", "on"},
		{"synchronous_commit", "on"},
		{"checkpoint_completion_target", "0.9"},
		{"checkpoint_timeout", "'3min'"},
		{"max_wal_size", "'512MB'"},
		{"min_wal_size", "'64MB'"},
		{"autovacuum", "off"},
		{"bgwriter_lru_maxpages", "0"},
		{"log_min_messages", "warning"},
		{"log_checkpoints", "on"},
		{"temp_buffers", "'4MB'"},
	};
#else
	static const workadb_guc_entry preset[] = {
		{"listen_addresses", "''"},
		{"unix_socket_directories", "'.sockets'"},
		{"shared_preload_libraries", "''"},
		{"max_connections", "5"},
		{"max_worker_processes", "0"},
		{"max_parallel_workers", "0"},
		{"max_parallel_maintenance_workers", "0"},
		{"max_parallel_workers_per_gather", "0"},
		{"max_replication_slots", "0"},
		{"max_logical_replication_workers", "0"},
		{"max_sync_workers_per_subscription", "0"},
		{"shared_buffers", "'64MB'"},
		{"work_mem", "'8MB'"},
		{"maintenance_work_mem", "'32MB'"},
		{"effective_io_concurrency", "0"},
		{"jit", "off"},
		{"wal_level", "minimal"},
		{"max_wal_senders", "0"},
		{"fsync", "on"},
		{"full_page_writes", "on"},
		{"synchronous_commit", "on"},
		{"checkpoint_completion_target", "0.9"},
		{"checkpoint_timeout", "'10min'"},
		{"max_wal_size", "'2GB'"},
		{"min_wal_size", "'80MB'"},
		{"autovacuum", "off"},
		{"bgwriter_lru_maxpages", "0"},
		{"log_min_messages", "warning"},
		{"log_checkpoints", "on"},
	};
#endif

	char conf_path[2048];
	char tmp_path[2048];
	char socket_dir[2048];
	FILE *in = NULL;
	FILE *out = NULL;
	char line[4096];
	size_t i;

	if (!pgdata || pgdata[0] == '\0')
	{
		wepg_set_error_buf(errbuf, errlen, "pgdata path missing");
		return 0;
	}

	if (snprintf(conf_path, sizeof(conf_path), "%s/postgresql.conf", pgdata) >= (int) sizeof(conf_path))
	{
		wepg_set_error_buf(errbuf, errlen, "postgresql.conf path too long");
		return 0;
	}
	if (snprintf(tmp_path, sizeof(tmp_path), "%s/postgresql.conf.workadb.tmp", pgdata) >= (int) sizeof(tmp_path))
	{
		wepg_set_error_buf(errbuf, errlen, "postgresql.conf temp path too long");
		return 0;
	}
	if (snprintf(socket_dir, sizeof(socket_dir), "%s/.sockets", pgdata) >= (int) sizeof(socket_dir))
	{
		wepg_set_error_buf(errbuf, errlen, "socket dir path too long");
		return 0;
	}

	if (!workadb_mkdir_p(socket_dir, 0700))
	{
		snprintf(errbuf, errlen, "failed to create socket dir: %s", strerror(errno));
		return 0;
	}

	in = fopen(conf_path, "r");
	out = fopen(tmp_path, "w");
	if (!out)
	{
		snprintf(errbuf, errlen, "failed to open temp config: %s", strerror(errno));
		if (in)
			fclose(in);
		return 0;
	}

	if (in)
	{
		while (fgets(line, sizeof(line), in))
		{
			int skip = 0;
			for (i = 0; i < sizeof(preset) / sizeof(preset[0]); i++)
			{
				if (workadb_line_matches_key(line, preset[i].key))
				{
					skip = 1;
					break;
				}
			}
			if (!skip)
				fputs(line, out);
		}
		fclose(in);
	}

	fputs("\n# Worka embedded preset (managed)\n", out);
	for (i = 0; i < sizeof(preset) / sizeof(preset[0]); i++)
		fprintf(out, "%s = %s\n", preset[i].key, preset[i].value);

	if (fclose(out) != 0)
	{
		snprintf(errbuf, errlen, "failed to write config: %s", strerror(errno));
		return 0;
	}

	if (rename(tmp_path, conf_path) != 0)
	{
		snprintf(errbuf, errlen, "failed to replace config: %s", strerror(errno));
		return 0;
	}

	return 1;
}

static int
workadb_ensure_pgdata_dirs(const char *pgdata, char *errbuf, size_t errlen)
{
	static const char *const subdirs[] = {
		"global",
		"pg_wal",
		"pg_wal/archive_status",
		"pg_wal/summaries",
		"pg_commit_ts",
		"pg_dynshmem",
		"pg_notify",
		"pg_serial",
		"pg_snapshots",
		"pg_subtrans",
		"pg_twophase",
		"pg_multixact",
		"pg_multixact/members",
		"pg_multixact/offsets",
		"base",
		"base/1",
		"pg_replslot",
		"pg_tblspc",
		"pg_stat",
		"pg_stat_tmp",
		"pg_xact",
		"pg_logical",
		"pg_logical/snapshots",
		"pg_logical/mappings"
	};
	size_t i;

	for (i = 0; i < sizeof(subdirs) / sizeof(subdirs[0]); i++)
	{
		char path[2048];
		snprintf(path, sizeof(path), "%s/%s", pgdata, subdirs[i]);
		if (!workadb_mkdir_p(path, 0700))
		{
			snprintf(errbuf, errlen, "mkdir failed for %s: %s", path, strerror(errno));
			return 0;
		}
	}

	return 1;
}

wepg_status
wepg_initdb_internal(struct wepg_engine *engine, char *errbuf, size_t errlen) {
	const char *template_path = getenv("WORKADB_TEMPLATE_PATH");

    if (!engine) {
        wepg_set_error_buf(errbuf, errlen, "invalid handle");
        return WEPG_ERR_INVALID_CONFIG;
    }

	if (template_path && template_path[0] != '\0')
	{
		if (!workadb_ensure_assets(engine, errbuf, errlen))
			return WEPG_ERR_INITDB_FAILED;
	}

    if (wepg_pgdata_exists(engine->pgdata_path)) {
        if (chmod(engine->pgdata_path, 0700) != 0)
        {
            wepg_set_error_buf(errbuf, errlen, "failed to chmod data dir");
            return WEPG_ERR_INITDB_FAILED;
        }
		if (!workadb_apply_embedded_preset(engine->pgdata_path, errbuf, errlen))
			return WEPG_ERR_INITDB_FAILED;
        wepg_set_error_buf(errbuf, errlen, NULL);
        return WEPG_OK;
    }

	if (template_path && template_path[0] != '\0')
	{
		if (!workadb_copy_dir(template_path, engine->pgdata_path,
							  errbuf, errlen))
		{
			if (errbuf[0] == '\0')
				wepg_set_error_buf(errbuf, errlen, "failed to copy template");
			return WEPG_ERR_INITDB_FAILED;
		}

		if (chmod(engine->pgdata_path, 0700) != 0)
		{
			wepg_set_error_buf(errbuf, errlen, "failed to chmod data dir");
			return WEPG_ERR_INITDB_FAILED;
		}

		if (!workadb_ensure_pgdata_dirs(engine->pgdata_path,
										errbuf,
										errlen))
		{
			if (errbuf[0] == '\0')
				wepg_set_error_buf(errbuf, errlen, "failed to create pgdata directories");
			return WEPG_ERR_INITDB_FAILED;
		}

		if (!workadb_apply_embedded_preset(engine->pgdata_path, errbuf, errlen))
			return WEPG_ERR_INITDB_FAILED;

		wepg_set_error_buf(errbuf, errlen, NULL);
		return WEPG_OK;
	}

	{
		char *argv[8];
		int argc = 0;
		char data_arg[PATH_MAX];

		if (snprintf(data_arg, sizeof(data_arg), "%s", engine->pgdata_path) >= (int) sizeof(data_arg))
		{
			wepg_set_error_buf(errbuf, errlen, "pgdata path too long");
			return WEPG_ERR_INITDB_FAILED;
		}

		argv[argc++] = "initdb";
		argv[argc++] = "-D";
		argv[argc++] = data_arg;
		argv[argc++] = "--no-sync";
		argv[argc] = NULL;

		if (workadb_initdb_run(argc, argv) != 0)
		{
			wepg_set_error_buf(errbuf, errlen, "initdb failed");
			return WEPG_ERR_INITDB_FAILED;
		}

		if (!workadb_apply_embedded_preset(engine->pgdata_path, errbuf, errlen))
			return WEPG_ERR_INITDB_FAILED;

		wepg_set_error_buf(errbuf, errlen, NULL);
		return WEPG_OK;
	}
}

wepg_status
wepg_initdb(wepg_handle handle) {
    struct wepg_engine *engine = (struct wepg_engine *) handle;
	wepg_task task;

    if (!engine) {
        return WEPG_ERR_INVALID_CONFIG;
    }

	memset(&task, 0, sizeof(task));
	task.type = WEPG_TASK_INITDB;
	task.engine = engine;
	wepg_task_init(&task);
	wepg_queue_task(engine->singleton, &task);
	wepg_wait_task(&task);

	wepg_set_error(engine, task.errbuf[0] == '\0' ? NULL : task.errbuf);
	return task.status;
}

wepg_status
wepg_step(wepg_handle handle, const wepg_request *request, wepg_response *response) {
    struct wepg_engine *engine = (struct wepg_engine *) handle;
	wepg_task task;
	uint8_t *sql_copy = NULL;

    if (!engine || !request || !response) {
        return WEPG_ERR_INVALID_CONFIG;
    }
    if (request->mode != WEPG_MODE_SQL) {
        wepg_set_error(engine, "wire mode not implemented");
        return WEPG_ERR_UNSUPPORTED;
    }
    if (!response->bytes || response->capacity == 0) {
        wepg_set_error(engine, "response buffer not provided");
        return WEPG_ERR_BUFFER_TOO_SMALL;
    }

	if (request->len > 0)
	{
		sql_copy = (uint8_t *) malloc(request->len);
		if (!sql_copy)
		{
			wepg_set_error(engine, "out of memory");
			return WEPG_ERR_INTERNAL;
		}
		memcpy(sql_copy, request->bytes, request->len);
	}

	memset(&task, 0, sizeof(task));
	task.type = WEPG_TASK_STEP;
	task.engine = engine;
	task.sql_bytes = sql_copy;
	task.sql_len = request->len;
	task.out_bytes = response->bytes;
	task.out_cap = response->capacity;
	wepg_task_init(&task);
	wepg_queue_task(engine->singleton, &task);
	wepg_wait_task(&task);
	if (sql_copy)
		free(sql_copy);

	response->len = task.out_len;
	response->rows = task.out_rows;
	wepg_set_error(engine, task.errbuf[0] == '\0' ? NULL : task.errbuf);
	if (task.status != WEPG_OK)
		return task.status;

    if (engine->max_response_bytes != 0 && response->len > engine->max_response_bytes) {
        return WEPG_ERR_BUFFER_TOO_SMALL;
    }

    return WEPG_OK;
}

wepg_status
wepg_step_params(wepg_handle handle, const wepg_request_params *request, wepg_response *response) {
    struct wepg_engine *engine = (struct wepg_engine *) handle;
	wepg_task task;
	uint8_t *sql_copy = NULL;

    if (!engine || !request || !response) {
        return WEPG_ERR_INVALID_CONFIG;
    }
    if (request->mode != WEPG_MODE_SQL) {
        wepg_set_error(engine, "wire mode not implemented");
        return WEPG_ERR_UNSUPPORTED;
    }
    if (!response->bytes || response->capacity == 0) {
        wepg_set_error(engine, "response buffer not provided");
        return WEPG_ERR_BUFFER_TOO_SMALL;
    }
	if (request->param_count > 0 && !request->params) {
		wepg_set_error(engine, "parameter buffer not provided");
		return WEPG_ERR_INVALID_CONFIG;
	}

	if (request->len > 0)
	{
		sql_copy = (uint8_t *) malloc(request->len);
		if (!sql_copy)
		{
			wepg_set_error(engine, "out of memory");
			return WEPG_ERR_INTERNAL;
		}
		memcpy(sql_copy, request->bytes, request->len);
	}

	memset(&task, 0, sizeof(task));
	task.type = WEPG_TASK_STEP;
	task.engine = engine;
	task.sql_bytes = sql_copy;
	task.sql_len = request->len;
	task.params = request->params;
	task.param_count = request->param_count;
	task.out_bytes = response->bytes;
	task.out_cap = response->capacity;
	wepg_task_init(&task);
	wepg_queue_task(engine->singleton, &task);
	wepg_wait_task(&task);
	if (sql_copy)
		free(sql_copy);

	response->len = task.out_len;
	response->rows = task.out_rows;
	wepg_set_error(engine, task.errbuf[0] == '\0' ? NULL : task.errbuf);
	if (task.status != WEPG_OK)
		return task.status;

    if (engine->max_response_bytes != 0 && response->len > engine->max_response_bytes) {
        return WEPG_ERR_BUFFER_TOO_SMALL;
    }

    return WEPG_OK;
}

wepg_status
wepg_reset(wepg_handle handle) {
    struct wepg_engine *engine = (struct wepg_engine *) handle;
	wepg_task task;

    if (!engine) {
        return WEPG_ERR_INVALID_CONFIG;
    }

	memset(&task, 0, sizeof(task));
	task.type = WEPG_TASK_RESET;
	task.engine = engine;
	wepg_task_init(&task);
	wepg_queue_task(engine->singleton, &task);
	wepg_wait_task(&task);
	wepg_set_error(engine, task.errbuf[0] == '\0' ? NULL : task.errbuf);
	return task.status;
}

wepg_status
wepg_shutdown(wepg_handle handle) {
    struct wepg_engine *engine = (struct wepg_engine *) handle;
	wepg_task task;
	bool is_last = false;

    if (!engine) {
        return WEPG_ERR_INVALID_CONFIG;
    }
	is_last = wepg_singleton_release(engine->singleton);
	if (is_last)
	{
		pthread_mutex_lock(&wepg_singleton_mutex);
		engine->singleton->shutting_down = true;
		pthread_mutex_unlock(&wepg_singleton_mutex);

		memset(&task, 0, sizeof(task));
		task.type = WEPG_TASK_SHUTDOWN;
		task.engine = engine;
		wepg_task_init(&task);
		pthread_mutex_lock(&engine->singleton->mutex);
		engine->singleton->worker_stop = true;
		if (engine->singleton->tail)
			engine->singleton->tail->next = &task;
		else
			engine->singleton->head = &task;
		engine->singleton->tail = &task;
		pthread_cond_signal(&engine->singleton->cond);
		pthread_mutex_unlock(&engine->singleton->mutex);
		wepg_wait_task(&task);
		wepg_set_error(engine, task.errbuf[0] == '\0' ? NULL : task.errbuf);
		wepg_singleton_destroy(engine->singleton);
	}
	else
	{
		wepg_set_error(engine, NULL);
	}
    free(engine->pgdata_path);
    free(engine->temp_path);
    free(engine);
    return WEPG_OK;
}

const char *
wepg_last_error(wepg_handle handle) {
    struct wepg_engine *engine = (struct wepg_engine *) handle;

    if (!engine) {
        return "workadb: invalid handle";
    }
    return engine->last_error;
}

uint32_t
worka_abi_version(void)
{
	return wepg_abi_version();
}

const char *
worka_version_string(void)
{
	return wepg_version_string();
}

void
worka_set_logger(worka_log_fn fn, void *ctx)
{
	wepg_set_logger(fn, ctx);
}

worka_status
worka_open(const worka_config *config, worka_handle *out)
{
	if (!config || !out || !config->pgdata_path || config->pgdata_path[0] == '\0')
		return WEPG_ERR_INVALID_CONFIG;

	if ((config->flags & WEPG_FLAG_NO_AUTO_INITDB) != 0 &&
		!wepg_pgdata_exists(config->pgdata_path))
	{
		return WEPG_ERR_INITDB_FAILED;
	}

	return wepg_init((const wepg_config *) config, (wepg_handle *) out);
}

worka_status
worka_step(worka_handle handle, const worka_request *request, worka_response *response)
{
	return wepg_step((wepg_handle) handle, (const wepg_request *) request, (wepg_response *) response);
}

worka_status
worka_step_params(worka_handle handle, const worka_request_params *request, worka_response *response)
{
	return wepg_step_params((wepg_handle) handle, (const wepg_request_params *) request, (wepg_response *) response);
}

worka_status
worka_reset(worka_handle handle)
{
	return wepg_reset((wepg_handle) handle);
}

worka_status
worka_shutdown(worka_handle handle)
{
	return wepg_shutdown((wepg_handle) handle);
}

const char *
worka_last_error(worka_handle handle)
{
	return wepg_last_error((wepg_handle) handle);
}
