#include "postgres.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <limits.h>

#include "libpq/libpq-be.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "utils/elog.h"
#include "utils/guc.h"
#include "storage/lock.h"
#include "utils/memutils.h"
#include "utils/timeout.h"
#include "utils/timestamp.h"
#include "utils/timeout.h"
#include "postmaster/postmaster.h"
#include "storage/fd.h"
#include "common/file_perm.h"
#include "catalog/pg_type.h"
#include "tcop/workadb_embed.h"
#include "access/xact.h"
#include "executor/spi.h"
#include "utils/lsyscache.h"
#include "storage/ipc.h"
#include "utils/snapmgr.h"
#include "access/xlog.h"
#include "port.h"

static bool workadb_backend_started = false;

static void
workadb_debug(const char *message)
{
	if (!message)
		return;
	if (!getenv("WORKADB_DEBUG"))
		return;
	if (CurrentMemoryContext != NULL)
		ereport(LOG, (errmsg("[workadb] %s", message)));
	else
		fprintf(stderr, "[workadb] %s\n", message);
}

static void
workadb_set_error(char *errbuf, size_t errlen, const char *message)
{
	if (!errbuf || errlen == 0)
		return;
	if (!message)
	{
		errbuf[0] = '\0';
		return;
	}
	snprintf(errbuf, errlen, "%s", message);
}

static ErrorData *
workadb_copy_error_data(void)
{
	MemoryContext oldcontext = CurrentMemoryContext;
	ErrorData  *edata;

	if (TopMemoryContext)
		oldcontext = MemoryContextSwitchTo(TopMemoryContext);

	edata = CopyErrorData();

	if (TopMemoryContext)
		MemoryContextSwitchTo(oldcontext);

	return edata;
}

static void
workadb_reset_message_context(void)
{
	MemoryContext oldcontext;

	if (MessageContext == NULL)
		return;

	oldcontext = MemoryContextSwitchTo(MessageContext);
	MemoryContextReset(MessageContext);
	MemoryContextSwitchTo(oldcontext);
}

static bool
workadb_sql_returns_rows(const uint8_t *sql_bytes, size_t sql_len)
{
	size_t i = 0;
	char keyword[16];
	size_t k = 0;

	if (!sql_bytes || sql_len == 0)
		return false;

	for (;;)
	{
		while (i < sql_len && isspace(sql_bytes[i]))
			i++;
		if (i + 1 < sql_len && sql_bytes[i] == '-' && sql_bytes[i + 1] == '-')
		{
			i += 2;
			while (i < sql_len && sql_bytes[i] != '\n')
				i++;
			continue;
		}
		if (i + 1 < sql_len && sql_bytes[i] == '/' && sql_bytes[i + 1] == '*')
		{
			i += 2;
			while (i + 1 < sql_len && !(sql_bytes[i] == '*' && sql_bytes[i + 1] == '/'))
				i++;
			if (i + 1 < sql_len)
				i += 2;
			continue;
		}
		break;
	}

	while (i < sql_len && isalpha(sql_bytes[i]) && k + 1 < sizeof(keyword))
	{
		keyword[k++] = (char) toupper(sql_bytes[i]);
		i++;
	}
	keyword[k] = '\0';

	if (strcmp(keyword, "SELECT") == 0 ||
		strcmp(keyword, "WITH") == 0 ||
		strcmp(keyword, "VALUES") == 0 ||
		strcmp(keyword, "INSERT") == 0 ||
		strcmp(keyword, "UPDATE") == 0 ||
		strcmp(keyword, "DELETE") == 0 ||
		strcmp(keyword, "EXPLAIN") == 0 ||
		strcmp(keyword, "SHOW") == 0)
		return true;

	return false;
}

static bool
workadb_sql_is_tx_control(const uint8_t *sql_bytes, size_t sql_len)
{
	size_t i = 0;
	char keyword[16];
	size_t k = 0;

	if (!sql_bytes || sql_len == 0)
		return false;

	for (;;)
	{
		while (i < sql_len && isspace(sql_bytes[i]))
			i++;
		if (i + 1 < sql_len && sql_bytes[i] == '-' && sql_bytes[i + 1] == '-')
		{
			i += 2;
			while (i < sql_len && sql_bytes[i] != '\n')
				i++;
			continue;
		}
		if (i + 1 < sql_len && sql_bytes[i] == '/' && sql_bytes[i + 1] == '*')
		{
			i += 2;
			while (i + 1 < sql_len && !(sql_bytes[i] == '*' && sql_bytes[i + 1] == '/'))
				i++;
			if (i + 1 < sql_len)
				i += 2;
			continue;
		}
		break;
	}

	while (i < sql_len && isalpha(sql_bytes[i]) && k + 1 < sizeof(keyword))
	{
		keyword[k++] = (char) toupper(sql_bytes[i]);
		i++;
	}
	keyword[k] = '\0';

	if (strcmp(keyword, "BEGIN") == 0 ||
		strcmp(keyword, "COMMIT") == 0 ||
		strcmp(keyword, "ROLLBACK") == 0 ||
		strcmp(keyword, "START") == 0 ||
		strcmp(keyword, "END") == 0)
		return true;

	return false;
}

static char *
workadb_dup_sql(const uint8_t *sql_bytes, size_t sql_len)
{
	MemoryContext oldcontext = CurrentMemoryContext;
	char	   *sql;

	if (TopMemoryContext)
		oldcontext = MemoryContextSwitchTo(TopMemoryContext);

	sql = pnstrdup((const char *) sql_bytes, sql_len);

	if (TopMemoryContext)
		MemoryContextSwitchTo(oldcontext);

	return sql;
}

static void
workadb_apply_embedded_gucs(void)
{
	SetConfigOption("listen_addresses", "", PGC_POSTMASTER, PGC_S_OVERRIDE);
	SetConfigOption("unix_socket_directories", "", PGC_POSTMASTER, PGC_S_OVERRIDE);
	SetConfigOption("shared_preload_libraries", "", PGC_POSTMASTER, PGC_S_OVERRIDE);
	SetConfigOption("max_worker_processes", "0", PGC_POSTMASTER, PGC_S_OVERRIDE);
	SetConfigOption("max_parallel_workers", "0", PGC_POSTMASTER, PGC_S_OVERRIDE);
	SetConfigOption("max_parallel_maintenance_workers", "0", PGC_POSTMASTER, PGC_S_OVERRIDE);
	SetConfigOption("max_parallel_workers_per_gather", "0", PGC_POSTMASTER, PGC_S_OVERRIDE);
	SetConfigOption("max_wal_senders", "0", PGC_POSTMASTER, PGC_S_OVERRIDE);
	SetConfigOption("max_replication_slots", "0", PGC_POSTMASTER, PGC_S_OVERRIDE);
	SetConfigOption("max_logical_replication_workers", "0", PGC_POSTMASTER, PGC_S_OVERRIDE);
	SetConfigOption("max_sync_workers_per_subscription", "0", PGC_POSTMASTER, PGC_S_OVERRIDE);
	SetConfigOption("wal_level", "minimal", PGC_POSTMASTER, PGC_S_OVERRIDE);
	SetConfigOption("max_connections", "20", PGC_POSTMASTER, PGC_S_OVERRIDE);
	SetConfigOption("shared_buffers", "32MB", PGC_POSTMASTER, PGC_S_OVERRIDE);
	SetConfigOption("jit", "off", PGC_POSTMASTER, PGC_S_OVERRIDE);
	SetConfigOption("shared_memory_type", "mmap", PGC_POSTMASTER, PGC_S_OVERRIDE);
	SetConfigOption("dynamic_shared_memory_type", "mmap", PGC_POSTMASTER, PGC_S_OVERRIDE);
}

static void
workadb_init_port(const char *database_name, const char *user_name)
{
	MemoryContext oldcontext = CurrentMemoryContext;

	if (MyProcPort)
		return;

	if (TopMemoryContext)
		oldcontext = MemoryContextSwitchTo(TopMemoryContext);

	MyProcPort = (Port *) palloc0(sizeof(Port));
	if (!MyProcPort)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("could not allocate workadb port")));

	MyProcPort->sock = PGINVALID_SOCKET;
	MyProcPort->noblock = true;
	MyProcPort->proto = PG_PROTOCOL(3, 0);
	MyProcPort->database_name = database_name ? pstrdup(database_name) : NULL;
	MyProcPort->user_name = user_name ? pstrdup(user_name) : NULL;

	if (TopMemoryContext)
		MemoryContextSwitchTo(oldcontext);
}

static bool
workadb_prepare_data_dir(const char *data_dir, char *errbuf, size_t errlen)
{
	struct stat stat_buf;
	char pgversion_path[MAXPGPATH];
	FILE *file;
	char file_version_string[64];
	char *endptr = NULL;
	long file_major = 0;
	long my_major = 0;

	if (!data_dir || data_dir[0] == '\0')
	{
		workadb_set_error(errbuf, errlen, "data directory path is empty");
		return false;
	}

	if (stat(data_dir, &stat_buf) != 0)
	{
		workadb_set_error(errbuf, errlen, "data directory does not exist");
		return false;
	}

	if (!S_ISDIR(stat_buf.st_mode))
	{
		workadb_set_error(errbuf, errlen, "data directory is not a directory");
		return false;
	}

#if !defined(WIN32) && !defined(__CYGWIN__)
	if (stat_buf.st_uid != geteuid())
	{
		workadb_set_error(errbuf, errlen, "data directory has wrong ownership");
		return false;
	}
#endif

#if !defined(WIN32) && !defined(__CYGWIN__)
	if (stat_buf.st_mode & PG_MODE_MASK_GROUP)
	{
		if (chmod(data_dir, 0700) != 0 || stat(data_dir, &stat_buf) != 0)
		{
			workadb_set_error(errbuf, errlen, "failed to fix data directory permissions");
			return false;
		}
	}
#endif

	SetDataDirectoryCreatePerm(stat_buf.st_mode);
	umask(pg_mode_mask);
	data_directory_mode = pg_dir_create_mode;

	snprintf(pgversion_path, sizeof(pgversion_path), "%s/PG_VERSION", data_dir);
	file = AllocateFile(pgversion_path, "r");
	if (!file)
	{
		workadb_set_error(errbuf, errlen, "PG_VERSION missing in data directory");
		return false;
	}

	file_version_string[0] = '\0';
	if (fscanf(file, "%63s", file_version_string) != 1)
	{
		FreeFile(file);
		workadb_set_error(errbuf, errlen, "PG_VERSION is invalid");
		return false;
	}
	FreeFile(file);

	my_major = strtol(PG_VERSION, &endptr, 10);
	file_major = strtol(file_version_string, &endptr, 10);
	if (my_major != file_major)
	{
		snprintf(errbuf, errlen,
				 "PG_VERSION mismatch for %s: data dir=%s (PG_VERSION=%s) server_major=%ld",
				 pgversion_path,
				 file_version_string,
				 PG_VERSION,
				 my_major);
		return false;
	}

	return true;
}

bool
workadb_backend_is_started(void)
{
	return workadb_backend_started;
}

bool
workadb_backend_start(const char *data_dir,
					  const char *database_name,
					  const char *user_name,
					  char *errbuf,
					  size_t errlen)
{
	if (workadb_backend_started)
		return true;

	if (!data_dir || !database_name || !user_name)
	{
		workadb_set_error(errbuf, errlen, "missing required backend parameters");
		return false;
	}

	PG_TRY();
	{
		const char *exec_override = getenv("WORKADB_EXEC_PATH");

		if (exec_override && exec_override[0] != '\0')
			strlcpy(my_exec_path, exec_override, MAXPGPATH);

		workadb_debug("MemoryContextInit");
		MemoryContextInit();

		workadb_debug("InitStandaloneProcess");
		InitStandaloneProcess("workadb");
		/* Keep startup errors catchable in embedded mode. */
		ExitOnAnyError = false;

		workadb_debug("InitializeGUCOptions");
		InitializeGUCOptions();
		workadb_debug("InitializeTimeouts");
		InitializeTimeouts();
		SetDataDir(data_dir);

		workadb_debug("SelectConfigFiles");
		if (!SelectConfigFiles(data_dir, "workadb"))
			ereport(ERROR,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("could not load configuration files")));
		workadb_debug("ApplyEmbeddedGucs");
		workadb_apply_embedded_gucs();

		workadb_debug("workadb_prepare_data_dir");
		if (!workadb_prepare_data_dir(data_dir, errbuf, errlen))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("%s", errbuf)));
		workadb_debug("ChangeToDataDir");
		ChangeToDataDir();
		workadb_debug("CreateDataDirLockFile");
		CreateDataDirLockFile(false);
		workadb_debug("LocalProcessControlFile");
		LocalProcessControlFile(false);

		workadb_debug("process_shared_preload_libraries");
		process_shared_preload_libraries();
		workadb_debug("InitializeMaxBackends");
		InitializeMaxBackends();
#if PG_VERSION_NUM >= 180000
		workadb_debug("InitPostmasterChildSlots");
		InitPostmasterChildSlots();
		workadb_debug("InitializeFastPathLocks");
		InitializeFastPathLocks();
#else
		/*
		 * Embedded mode runs the backend as a standalone process (no postmaster
		 * parent and no death-monitoring pipe). Calling InitPostmasterChild()
		 * would set IsUnderPostmaster=true and attempt to use
		 * postmaster_alive_fds, leading to FATAL "read on postmaster death
		 * monitoring pipe failed" on platforms like macOS/iOS.
		 */
		workadb_debug("SkipInitPostmasterChild");
#endif
		workadb_debug("process_shmem_requests");
		process_shmem_requests();
		workadb_debug("InitializeShmemGUCs");
		InitializeShmemGUCs();
		workadb_debug("InitializeWalConsistencyChecking");
		InitializeWalConsistencyChecking();
		workadb_debug("CreateSharedMemoryAndSemaphores");
		CreateSharedMemoryAndSemaphores();
		workadb_debug("set_max_safe_fds");
		set_max_safe_fds();

		workadb_debug("InitProcess");
		MyProcPid = getpid();
		PgStartTime = GetCurrentTimestamp();
		InitProcess();

		workadb_debug("workadb_init_port");
		workadb_init_port(database_name, user_name);
		whereToSendOutput = DestNone;
		SetProcessingMode(InitProcessing);

		/*
		 * Postgres expects timeout.c to be initialized in every backend.
		 * Normally this is done in PostgresMain(); embedded mode doesn't call
		 * that, so we must do it here to avoid corrupted timeout bookkeeping
		 * (e.g. "timeout index -1 out of range").
		 */
		workadb_debug("InitializeTimeouts");
		InitializeTimeouts();

		workadb_debug("BaseInit");
		BaseInit();
		sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);

		workadb_debug("InitPostgres");
		InitPostgres(database_name, InvalidOid,
					 user_name, InvalidOid,
					 INIT_PG_LOAD_SESSION_LIBS,
					 NULL);
		if (RecoveryInProgress())
		{
			time_t start = time(NULL);
			const int max_wait_seconds = 60;
			workadb_debug("RecoveryInProgress wait");
			while (RecoveryInProgress())
			{
				if ((time(NULL) - start) > max_wait_seconds)
					ereport(ERROR,
							(errcode(ERRCODE_INTERNAL_ERROR),
							 errmsg("recovery timeout")));
				pg_usleep(100000L);
			}
		}

		workadb_debug("InitMessageContext");
		if (MessageContext == NULL)
			MessageContext = AllocSetContextCreate(TopMemoryContext,
												   "MessageContext",
												   ALLOCSET_DEFAULT_SIZES);

		workadb_debug("PostmasterContext cleanup");
		if (PostmasterContext)
		{
			MemoryContextSwitchTo(TopMemoryContext);
			MemoryContextDelete(PostmasterContext);
			PostmasterContext = NULL;
		}

		workadb_debug("SetProcessingMode");
		SetProcessingMode(NormalProcessing);
		workadb_debug("BeginReportingGUCOptions");
		BeginReportingGUCOptions();
		workadb_debug("pgstat_report_connect");
		pgstat_report_connect(MyDatabaseId);

		workadb_backend_started = true;
		workadb_debug("backend started");
	}
	PG_CATCH();
	{
		ErrorData  *edata = workadb_copy_error_data();

		FlushErrorState();
		workadb_backend_started = false;
		workadb_set_error(errbuf, errlen, edata->message);
		FreeErrorData(edata);
		return false;
	}
	PG_END_TRY();

	workadb_set_error(errbuf, errlen, NULL);
	return true;
}

bool
workadb_backend_exec(const char *sql, char *errbuf, size_t errlen)
{
	if (getenv("WORKADB_DEBUG") && sql)
	{
		char msg[256];
		snprintf(msg, sizeof(msg), "backend exec start len=%zu", strlen(sql));
		workadb_debug(msg);
	}
	if (!workadb_backend_started)
	{
		workadb_set_error(errbuf, errlen, "backend not initialized");
		return false;
	}
	if (!sql)
	{
		workadb_set_error(errbuf, errlen, "missing sql");
		return false;
	}

	PG_TRY();
	{
		workadb_exec_simple_query(sql);
	}
	PG_CATCH();
	{
		ErrorData  *edata = workadb_copy_error_data();

		FlushErrorState();
		workadb_set_error(errbuf, errlen, edata->message);
		FreeErrorData(edata);
		workadb_reset_message_context();
		return false;
	}
	PG_END_TRY();

	workadb_reset_message_context();
	workadb_set_error(errbuf, errlen, NULL);
	if (getenv("WORKADB_DEBUG"))
		workadb_debug("backend exec done");
	return true;
}

void
workadb_backend_shutdown(void)
{
	if (!workadb_backend_started)
		return;

	PG_TRY();
	{
		if (getenv("WORKADB_DEBUG"))
			workadb_debug("backend shutdown start");
		AbortOutOfAnyTransaction();
		shmem_exit(0);
		if (getenv("WORKADB_DEBUG"))
			workadb_debug("backend shutdown complete");
	}
	PG_CATCH();
	{
		FlushErrorState();
	}
	PG_END_TRY();

	workadb_backend_started = false;
	MyProcPort = NULL;
}

typedef struct WorkadbFrameWriter
{
	uint8_t    *buf;
	size_t		cap;
	size_t		len;
	bool		overflow;
	size_t		required;
} WorkadbFrameWriter;

static void
workadb_frame_write(WorkadbFrameWriter *writer, const void *data, size_t len)
{
	if (!writer || !data || len == 0)
		return;

	if (writer->len + len <= writer->cap && !writer->overflow)
		memcpy(writer->buf + writer->len, data, len);
	else
		writer->overflow = true;

	writer->len += len;
	writer->required += len;
}

static void
workadb_frame_write_u32(WorkadbFrameWriter *writer, uint32_t value)
{
	uint8_t buf[4];

	buf[0] = (uint8_t) ((value >> 24) & 0xff);
	buf[1] = (uint8_t) ((value >> 16) & 0xff);
	buf[2] = (uint8_t) ((value >> 8) & 0xff);
	buf[3] = (uint8_t) (value & 0xff);
	workadb_frame_write(writer, buf, sizeof(buf));
}

static void
workadb_frame_write_i32(WorkadbFrameWriter *writer, int32_t value)
{
	workadb_frame_write_u32(writer, (uint32_t) value);
}

static void
workadb_frame_write_i16(WorkadbFrameWriter *writer, int16_t value)
{
	uint16_t uvalue = (uint16_t) value;
	uint8_t buf[2];

	buf[0] = (uint8_t) ((uvalue >> 8) & 0xff);
	buf[1] = (uint8_t) (uvalue & 0xff);
	workadb_frame_write(writer, buf, sizeof(buf));
}

bool
workadb_backend_exec_sql(const uint8_t *sql_bytes,
						 size_t sql_len,
						 uint8_t *out,
						 size_t out_cap,
						 size_t *out_len,
						 int64_t *rows,
						 char *errbuf,
						 size_t errlen)
{
	WorkadbFrameWriter writer;
	char	   *sql = NULL;
	uint32_t col_count = 0;
	uint64_t row_count = 0;
	TupleDesc tupdesc = NULL;
	SPITupleTable *tuptable = NULL;
	int rc;
	bool spi_connected = false;
	bool snapshot_pushed = false;

	if (!workadb_backend_started)
	{
		workadb_set_error(errbuf, errlen, "backend not initialized");
		return false;
	}
	if (!sql_bytes || sql_len == 0 || !out || !out_len || !rows)
	{
		workadb_set_error(errbuf, errlen, "invalid sql execution parameters");
		return false;
	}
	if (IsAbortedTransactionBlockState())
	{
		workadb_set_error(errbuf, errlen,
						  "current transaction is aborted, commands ignored until end of transaction block");
		return false;
	}

	memset(&writer, 0, sizeof(writer));
	writer.buf = out;
	writer.cap = out_cap;
	writer.required = 0;

	if (getenv("WORKADB_DEBUG"))
	{
		char msg[256];
		bool expect_rows = workadb_sql_returns_rows(sql_bytes, sql_len);
		snprintf(msg, sizeof(msg),
				 "exec_sql SPI path expect_rows=%d len=%zu",
				 expect_rows ? 1 : 0,
				 sql_len);
		workadb_debug(msg);
	}

	if (workadb_sql_is_tx_control(sql_bytes, sql_len))
	{
		if (getenv("WORKADB_DEBUG"))
			workadb_debug("exec_sql simple path tx control");

		sql = workadb_dup_sql(sql_bytes, sql_len);
		if (!workadb_backend_exec(sql, errbuf, errlen))
		{
			pfree(sql);
			return false;
		}
		pfree(sql);
		sql = NULL;

		workadb_frame_write_u32(&writer, 1);
		workadb_frame_write_u32(&writer, 0);
		workadb_frame_write_u32(&writer, 0);

		*out_len = writer.required;
		*rows = 0;
		workadb_set_error(errbuf, errlen, NULL);
		return true;
	}

	PG_TRY();
	{
		StartTransactionCommand();
		SetCurrentStatementStartTimestamp();
		PushActiveSnapshot(GetTransactionSnapshot());
		snapshot_pushed = true;

		rc = SPI_connect();
		if (rc != SPI_OK_CONNECT)
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("SPI_connect failed")));
		spi_connected = true;

		sql = workadb_dup_sql(sql_bytes, sql_len);
		rc = SPI_execute(sql, false, 0);
		if (rc < 0)
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("SPI_execute failed: %s", SPI_result_code_string(rc))));
		pfree(sql);
		sql = NULL;

		tuptable = SPI_tuptable;
		row_count = SPI_processed;
		if (tuptable)
		{
			tupdesc = tuptable->tupdesc;
			col_count = (uint32_t) tupdesc->natts;
		}

		workadb_frame_write_u32(&writer, 1);
		workadb_frame_write_u32(&writer, col_count);
		workadb_frame_write_u32(&writer, (row_count > UINT32_MAX) ? 0 : (uint32_t) row_count);

		if (tupdesc)
		{
			for (uint32_t col = 0; col < col_count; col++)
			{
				Form_pg_attribute attr = TupleDescAttr(tupdesc, col);
				const char *name = NameStr(attr->attname);
				uint32_t name_len = (uint32_t) strlen(name);
				int32 typmod = attr->atttypmod;
				Oid typoid = getBaseTypeAndTypmod(attr->atttypid, &typmod);
//				if (getenv("WORKADB_DEBUG"))
//				{
//					char msg[256];
//					snprintf(msg, sizeof(msg),
//							 "result column %u name=%s atttypid=%u base=%u typmod=%d attlen=%d",
//							 col,
//							 name,
//							 (unsigned int) attr->atttypid,
//							 (unsigned int) typoid,
//							 typmod,
//							 (int) attr->attlen);
//					workadb_debug(msg);
//				}

				workadb_frame_write_u32(&writer, name_len);
				workadb_frame_write(&writer, name, name_len);
				workadb_frame_write_u32(&writer, (uint32_t) typoid);
				workadb_frame_write_i16(&writer, attr->attlen);
				workadb_frame_write_i32(&writer, typmod);
			}

			for (uint64_t row = 0; row < row_count; row++)
			{
				HeapTuple tuple = tuptable->vals[row];

				for (uint32_t col = 0; col < col_count; col++)
				{
					char *value = SPI_getvalue(tuple, tupdesc, (int) (col + 1));

					if (!value)
					{
//						if (getenv("WORKADB_DEBUG"))
//						{
//							Form_pg_attribute attr = TupleDescAttr(tupdesc, col);
//							const char *name = NameStr(attr->attname);
//							char msg[256];
//							snprintf(msg, sizeof(msg),
//									 "result row %llu col %u name=%s is NULL (atttypid=%u)",
//									 (unsigned long long) row,
//									 col,
//									 name,
//									 (unsigned int) attr->atttypid);
//							workadb_debug(msg);
//						}
						workadb_frame_write_i32(&writer, -1);
						continue;
					}

					workadb_frame_write_i32(&writer, (int32_t) strlen(value));
					workadb_frame_write(&writer, value, strlen(value));
				}
			}
		}

		if (spi_connected)
		{
			SPI_finish();
			spi_connected = false;
		}
		if (snapshot_pushed)
		{
			PopActiveSnapshot();
			snapshot_pushed = false;
		}
		CommitTransactionCommand();
	}
	PG_CATCH();
	{
		ErrorData  *edata = workadb_copy_error_data();
		int			elevel = edata ? edata->elevel : ERROR;

		if (sql)
		{
			pfree(sql);
			sql = NULL;
		}
		if (spi_connected)
			SPI_finish();
		if (snapshot_pushed && ActiveSnapshotSet())
			PopActiveSnapshot();

		/*
		 * In embedded mode, FATAL/PANIC are rethrown instead of exiting the
		 * host process. Bubble them up to the outer embedder handler so it can
		 * reset the embedded backend safely.
		 */
		if (elevel >= FATAL && getenv("WORKADB_EMBEDDED") != NULL && PG_exception_stack != NULL)
		{
			if (edata)
				FreeErrorData(edata);
			PG_RE_THROW();
		}

		AbortCurrentTransaction();
		FlushErrorState();
		workadb_set_error(errbuf, errlen, edata->message);
		FreeErrorData(edata);
		workadb_reset_message_context();
		return false;
	}
	PG_END_TRY();

	workadb_reset_message_context();
	if (writer.overflow)
	{
		workadb_set_error(errbuf, errlen, "result frame overflow");
		*out_len = 0;
		*rows = 0;
		return false;
	}

	*out_len = writer.required;
	*rows = (int64_t) row_count;
	workadb_set_error(errbuf, errlen, NULL);
	return true;
}

bool
workadb_backend_exec_sql_params(const uint8_t *sql_bytes,
								size_t sql_len,
								const wepg_param *params,
								size_t param_count,
								uint8_t *out,
								size_t out_cap,
								size_t *out_len,
								int64_t *rows,
								char *errbuf,
								size_t errlen)
{
	WorkadbFrameWriter writer;
	char	   *sql = NULL;
	uint32_t col_count = 0;
	uint64_t row_count = 0;
	TupleDesc tupdesc = NULL;
	SPITupleTable *tuptable = NULL;
	int rc;
	bool spi_connected = false;
	bool snapshot_pushed = false;
	Oid		   *argtypes = NULL;
	Datum	   *values = NULL;
	char	   *nulls = NULL;

	if (!params || param_count == 0)
		return workadb_backend_exec_sql(sql_bytes, sql_len, out, out_cap, out_len, rows, errbuf, errlen);

	if (!workadb_backend_started)
	{
		workadb_set_error(errbuf, errlen, "backend not initialized");
		return false;
	}
	if (!sql_bytes || sql_len == 0 || !out || !out_len || !rows)
	{
		workadb_set_error(errbuf, errlen, "invalid sql execution parameters");
		return false;
	}
	if (param_count > INT_MAX)
	{
		workadb_set_error(errbuf, errlen, "too many parameters");
		return false;
	}
	if (IsAbortedTransactionBlockState())
	{
		workadb_set_error(errbuf, errlen,
						  "current transaction is aborted, commands ignored until end of transaction block");
		return false;
	}

	memset(&writer, 0, sizeof(writer));
	writer.buf = out;
	writer.cap = out_cap;
	writer.required = 0;

	PG_TRY();
	{
		StartTransactionCommand();
		SetCurrentStatementStartTimestamp();
		PushActiveSnapshot(GetTransactionSnapshot());
		snapshot_pushed = true;

		rc = SPI_connect();
		if (rc != SPI_OK_CONNECT)
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("SPI_connect failed")));
		spi_connected = true;

		sql = workadb_dup_sql(sql_bytes, sql_len);

		argtypes = (Oid *) palloc(sizeof(Oid) * param_count);
		values = (Datum *) palloc(sizeof(Datum) * param_count);
		nulls = (char *) palloc(sizeof(char) * param_count);

		for (size_t i = 0; i < param_count; i++)
		{
			Oid typoid = (Oid) params[i].type_oid;

			if (params[i].is_null)
			{
				if (typoid == InvalidOid)
					typoid = UNKNOWNOID;
				argtypes[i] = typoid;
				values[i] = (Datum) 0;
				nulls[i] = 'n';
				continue;
			}

			if (typoid == InvalidOid)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("parameter type required for non-null value")));
			argtypes[i] = typoid;

			if (!params[i].value)
				ereport(ERROR,
						(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
						 errmsg("parameter value missing")));
			{
				Oid typinput;
				Oid typioparam;
				char *input = pnstrdup((const char *) params[i].value, params[i].len);

				getTypeInputInfo(typoid, &typinput, &typioparam);
				values[i] = OidInputFunctionCall(typinput, input, typioparam, -1);
				nulls[i] = ' ';
			}
		}

		rc = SPI_execute_with_args(sql, (int) param_count, argtypes, values, nulls, false, 0);
		if (rc < 0)
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("SPI_execute_with_args failed: %s", SPI_result_code_string(rc))));
		pfree(sql);
		sql = NULL;

		tuptable = SPI_tuptable;
		row_count = SPI_processed;
		if (tuptable)
		{
			tupdesc = tuptable->tupdesc;
			col_count = (uint32_t) tupdesc->natts;
		}

		workadb_frame_write_u32(&writer, 1);
		workadb_frame_write_u32(&writer, col_count);
		workadb_frame_write_u32(&writer, (row_count > UINT32_MAX) ? 0 : (uint32_t) row_count);

		if (tupdesc)
		{
			for (uint32_t col = 0; col < col_count; col++)
			{
				Form_pg_attribute attr = TupleDescAttr(tupdesc, col);
				const char *name = NameStr(attr->attname);
				uint32_t name_len = (uint32_t) strlen(name);
				int32 typmod = attr->atttypmod;
				Oid typoid = getBaseTypeAndTypmod(attr->atttypid, &typmod);
//				if (getenv("WORKADB_DEBUG"))
//				{
//					char msg[256];
//					snprintf(msg, sizeof(msg),
//							 "result column %u name=%s atttypid=%u base=%u typmod=%d attlen=%d",
//							 col,
//							 name,
//							 (unsigned int) attr->atttypid,
//							 (unsigned int) typoid,
//							 typmod,
//							 (int) attr->attlen);
//					workadb_debug(msg);
//				}

				workadb_frame_write_u32(&writer, name_len);
				workadb_frame_write(&writer, name, name_len);
				workadb_frame_write_u32(&writer, (uint32_t) typoid);
				workadb_frame_write_i16(&writer, attr->attlen);
				workadb_frame_write_i32(&writer, typmod);
			}

			for (uint64_t row = 0; row < row_count; row++)
			{
				HeapTuple tuple = tuptable->vals[row];

				for (uint32_t col = 0; col < col_count; col++)
				{
					char *value = SPI_getvalue(tuple, tupdesc, (int) (col + 1));

					if (!value)
					{
//						if (getenv("WORKADB_DEBUG"))
//						{
//							Form_pg_attribute attr = TupleDescAttr(tupdesc, col);
//							const char *name = NameStr(attr->attname);
//							char msg[256];
//							snprintf(msg, sizeof(msg),
//									 "result row %llu col %u name=%s is NULL (atttypid=%u)",
//									 (unsigned long long) row,
//									 col,
//									 name,
//									 (unsigned int) attr->atttypid);
//							workadb_debug(msg);
//						}
						workadb_frame_write_i32(&writer, -1);
						continue;
					}

					workadb_frame_write_i32(&writer, (int32_t) strlen(value));
					workadb_frame_write(&writer, value, strlen(value));
				}
			}
		}

		if (spi_connected)
		{
			SPI_finish();
			spi_connected = false;
		}
		if (snapshot_pushed)
		{
			PopActiveSnapshot();
			snapshot_pushed = false;
		}
		CommitTransactionCommand();
	}
	PG_CATCH();
	{
		ErrorData  *edata = workadb_copy_error_data();
		int			elevel = edata ? edata->elevel : ERROR;

		if (sql)
		{
			pfree(sql);
			sql = NULL;
		}
		if (spi_connected)
			SPI_finish();
		if (snapshot_pushed && ActiveSnapshotSet())
			PopActiveSnapshot();

		if (elevel >= FATAL && getenv("WORKADB_EMBEDDED") != NULL && PG_exception_stack != NULL)
		{
			if (edata)
				FreeErrorData(edata);
			PG_RE_THROW();
		}

		AbortCurrentTransaction();
		FlushErrorState();
		workadb_set_error(errbuf, errlen, edata->message);
		FreeErrorData(edata);
		workadb_reset_message_context();
		return false;
	}
	PG_END_TRY();

	workadb_reset_message_context();
	if (writer.overflow)
	{
		workadb_set_error(errbuf, errlen, "result frame overflow");
		*out_len = 0;
		*rows = 0;
		return false;
	}

	*out_len = writer.required;
	*rows = (int64_t) row_count;
	workadb_set_error(errbuf, errlen, NULL);
	return true;
}
