/*-------------------------------------------------------------------------
 *
 * pg_enhanced_query_logging.c
 *
 * Enhanced query logging extension for PostgreSQL.
 *
 * Produces slow query log output compatible with pt-query-digest, modeled
 * after Percona Server's improved slow query log.  The log is written to a
 * dedicated file (decoupled from the PostgreSQL error log) in the same
 * directory as the server's log_directory.
 *
 * The extension hooks into the executor pipeline to capture timing, I/O,
 * WAL, and row-count metrics for every query that exceeds a configurable
 * duration threshold.  Behaviour is tunable via GUC variables prefixed
 * with "peql.".
 *
 * Copyright (c) 2026, Agustin Gallego
 *
 * IDENTIFICATION
 *   pg_enhanced_query_logging/pg_enhanced_query_logging.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

#include "executor/executor.h"
#include "executor/instrument.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "libpq/libpq-be.h"
#include "miscadmin.h"
#include "pgtime.h"
#include "postmaster/syslogger.h"
#include "storage/fd.h"
#include "utils/guc.h"
#include "utils/timestamp.h"

PG_MODULE_MAGIC_EXT(
	.name = "pg_enhanced_query_logging",
	.version = "1.0"
);

/* ---------- GUC variables ------------------------------------------------ */

/* Master on/off switch */
static bool peql_enabled = true;

/*
 * Minimum execution time (in ms) for a query to be logged.
 * -1 disables logging entirely; 0 logs every query.
 */
static int peql_log_min_duration = -1;

/*
 * Override for the log directory.  When empty, the extension uses
 * PostgreSQL's log_directory (from the syslogger).
 */
static char *peql_log_directory = NULL;

/* Name of the slow query log file written by this extension. */
static char *peql_log_filename = NULL;

/* Verbosity levels for the log output. */
typedef enum
{
	PEQL_LOG_VERBOSITY_MINIMAL = 0,
	PEQL_LOG_VERBOSITY_STANDARD,
	PEQL_LOG_VERBOSITY_FULL
} PeqlLogVerbosity;

static const struct config_enum_entry verbosity_options[] = {
	{"minimal", PEQL_LOG_VERBOSITY_MINIMAL, false},
	{"standard", PEQL_LOG_VERBOSITY_STANDARD, false},
	{"full", PEQL_LOG_VERBOSITY_FULL, false},
	{NULL, 0, false}
};

static int peql_log_verbosity = PEQL_LOG_VERBOSITY_STANDARD;

/* Whether to log utility (DDL) statements. */
static bool peql_log_utility = false;

/* Whether to log nested statements (e.g. inside PL/pgSQL functions). */
static bool peql_log_nested = false;

/* ---------- Hook infrastructure ------------------------------------------ */

/* Nesting depth -- incremented around ExecutorRun/Finish calls. */
static int nesting_level = 0;

/*
 * Convenience macro: the extension is "active" when it is enabled, the
 * duration threshold is set (>= 0), and we are either at the top-level
 * statement or nested-statement logging is on.
 */
#define peql_active() \
	(peql_enabled && peql_log_min_duration >= 0 && \
	 (nesting_level == 0 || peql_log_nested))

/* Saved previous hook values so we can chain properly. */
static ExecutorStart_hook_type  prev_ExecutorStart  = NULL;
static ExecutorRun_hook_type    prev_ExecutorRun    = NULL;
static ExecutorFinish_hook_type prev_ExecutorFinish = NULL;
static ExecutorEnd_hook_type    prev_ExecutorEnd    = NULL;

/* Forward declarations for hook functions. */
static void peql_ExecutorStart(QueryDesc *queryDesc, int eflags);
static void peql_ExecutorRun(QueryDesc *queryDesc,
							 ScanDirection direction,
							 uint64 count);
static void peql_ExecutorFinish(QueryDesc *queryDesc);
static void peql_ExecutorEnd(QueryDesc *queryDesc);

/* Forward declarations for log formatting and file I/O. */
static void peql_write_log_entry(QueryDesc *queryDesc, double duration_ms);
static void peql_resolve_log_path(char *result, size_t resultsize);
static void peql_format_entry(StringInfo buf, QueryDesc *queryDesc,
							  double duration_ms);
static void peql_flush_to_file(const char *data, int len);

/* SQL-callable reset function. */
PG_FUNCTION_INFO_V1(pg_enhanced_query_logging_reset);

/*
 * ──────────────────────────────────────────────────────────────────────────
 * Module load callback
 *
 * Registers all GUC variables and installs executor hooks.  Called once
 * when the module is loaded via shared_preload_libraries (or LOAD).
 * ──────────────────────────────────────────────────────────────────────────
 */
void
_PG_init(void)
{
	/* ---- GUC: peql.enabled ---- */
	DefineCustomBoolVariable("peql.enabled",
							 "Enable or disable enhanced query logging.",
							 NULL,
							 &peql_enabled,
							 true,
							 PGC_SUSET,
							 0,
							 NULL, NULL, NULL);

	/* ---- GUC: peql.log_min_duration ---- */
	DefineCustomIntVariable("peql.log_min_duration",
							"Minimum execution time (ms) for a query to be logged.",
							"Queries finishing faster than this are not logged. "
							"-1 disables logging entirely; 0 logs all queries.",
							&peql_log_min_duration,
							-1,
							-1, INT_MAX,
							PGC_SUSET,
							GUC_UNIT_MS,
							NULL, NULL, NULL);

	/* ---- GUC: peql.log_directory ---- */
	DefineCustomStringVariable("peql.log_directory",
							   "Directory for the slow query log file.",
							   "When empty (the default), uses PostgreSQL's "
							   "log_directory setting.",
							   &peql_log_directory,
							   "",
							   PGC_SIGHUP,
							   0,
							   NULL, NULL, NULL);

	/* ---- GUC: peql.log_filename ---- */
	DefineCustomStringVariable("peql.log_filename",
							   "Name of the slow query log file.",
							   NULL,
							   &peql_log_filename,
							   "peql-slow.log",
							   PGC_SIGHUP,
							   0,
							   NULL, NULL, NULL);

	/* ---- GUC: peql.log_verbosity ---- */
	DefineCustomEnumVariable("peql.log_verbosity",
							 "Amount of detail written to the slow query log.",
							 "minimal: timing + rows only.  "
							 "standard: adds schema, thread_id, bytes.  "
							 "full: adds buffer/WAL/plan/JIT metrics.",
							 &peql_log_verbosity,
							 PEQL_LOG_VERBOSITY_STANDARD,
							 verbosity_options,
							 PGC_SUSET,
							 0,
							 NULL, NULL, NULL);

	/* ---- GUC: peql.log_utility ---- */
	DefineCustomBoolVariable("peql.log_utility",
							 "Log utility (DDL) statements.",
							 NULL,
							 &peql_log_utility,
							 false,
							 PGC_SUSET,
							 0,
							 NULL, NULL, NULL);

	/* ---- GUC: peql.log_nested ---- */
	DefineCustomBoolVariable("peql.log_nested",
							 "Log nested statements (inside functions).",
							 NULL,
							 &peql_log_nested,
							 false,
							 PGC_SUSET,
							 0,
							 NULL, NULL, NULL);

	/* Reserve the "peql" GUC prefix so no other extension can claim it. */
	MarkGUCPrefixReserved("peql");

	/* ---- Install executor hooks ---- */
	prev_ExecutorStart  = ExecutorStart_hook;
	ExecutorStart_hook  = peql_ExecutorStart;

	prev_ExecutorRun    = ExecutorRun_hook;
	ExecutorRun_hook    = peql_ExecutorRun;

	prev_ExecutorFinish = ExecutorFinish_hook;
	ExecutorFinish_hook = peql_ExecutorFinish;

	prev_ExecutorEnd    = ExecutorEnd_hook;
	ExecutorEnd_hook    = peql_ExecutorEnd;
}

/*
 * ──────────────────────────────────────────────────────────────────────────
 * ExecutorStart hook
 *
 * If the extension is active, enable full instrumentation on this query
 * so we can collect timing, buffer, and WAL metrics when it finishes.
 * ──────────────────────────────────────────────────────────────────────────
 */
static void
peql_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	/* Chain to any previously-installed hook, or the standard function. */
	if (prev_ExecutorStart)
		prev_ExecutorStart(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);

	/*
	 * When active, ensure totaltime instrumentation is allocated so we can
	 * measure overall execution duration in ExecutorEnd.  We ask for
	 * INSTRUMENT_ALL to also capture buffer and WAL counters for later
	 * phases.
	 */
	if (peql_active())
	{
		if (queryDesc->totaltime == NULL)
		{
			MemoryContext oldcxt;

			oldcxt = MemoryContextSwitchTo(queryDesc->estate->es_query_cxt);
			queryDesc->totaltime = InstrAlloc(1, INSTRUMENT_ALL, false);
			MemoryContextSwitchTo(oldcxt);
		}
	}
}

/*
 * ──────────────────────────────────────────────────────────────────────────
 * ExecutorRun hook
 *
 * Track nesting depth so we can distinguish top-level queries from
 * statements executed inside PL/pgSQL or other procedural languages.
 * ──────────────────────────────────────────────────────────────────────────
 */
static void
peql_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction, uint64 count)
{
	nesting_level++;
	PG_TRY();
	{
		if (prev_ExecutorRun)
			prev_ExecutorRun(queryDesc, direction, count);
		else
			standard_ExecutorRun(queryDesc, direction, count);
	}
	PG_FINALLY();
	{
		nesting_level--;
	}
	PG_END_TRY();
}

/*
 * ──────────────────────────────────────────────────────────────────────────
 * ExecutorFinish hook
 *
 * Same nesting-depth tracking as ExecutorRun.
 * ──────────────────────────────────────────────────────────────────────────
 */
static void
peql_ExecutorFinish(QueryDesc *queryDesc)
{
	nesting_level++;
	PG_TRY();
	{
		if (prev_ExecutorFinish)
			prev_ExecutorFinish(queryDesc);
		else
			standard_ExecutorFinish(queryDesc);
	}
	PG_FINALLY();
	{
		nesting_level--;
	}
	PG_END_TRY();
}

/*
 * ──────────────────────────────────────────────────────────────────────────
 * ExecutorEnd hook
 *
 * After the query has finished executing, check whether its duration
 * exceeds the configured threshold and, if so, write a log entry.
 * ──────────────────────────────────────────────────────────────────────────
 */
static void
peql_ExecutorEnd(QueryDesc *queryDesc)
{
	if (queryDesc->totaltime && peql_active())
	{
		double msec;

		/* Finalize the instrumentation counters. */
		InstrEndLoop(queryDesc->totaltime);

		msec = queryDesc->totaltime->total * 1000.0;

		if (msec >= peql_log_min_duration)
			peql_write_log_entry(queryDesc, msec);
	}

	/* Chain to previous hook or the standard cleanup. */
	if (prev_ExecutorEnd)
		prev_ExecutorEnd(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);
}

/*
 * ──────────────────────────────────────────────────────────────────────────
 * Log path resolution
 *
 * Builds the full filesystem path for the slow query log file.  Uses
 * peql.log_directory if set, otherwise falls back to the PostgreSQL
 * log_directory GUC.  Relative paths are resolved against DataDir.
 * ──────────────────────────────────────────────────────────────────────────
 */
static void
peql_resolve_log_path(char *result, size_t resultsize)
{
	const char *dir;
	const char *fname;

	/* Pick the directory: our override, or PostgreSQL's log_directory. */
	if (peql_log_directory && peql_log_directory[0] != '\0')
		dir = peql_log_directory;
	else
		dir = Log_directory;

	fname = (peql_log_filename && peql_log_filename[0] != '\0')
		? peql_log_filename : "peql-slow.log";

	/* Absolute paths are used as-is; relative paths resolve to DataDir. */
	if (is_absolute_path(dir))
		snprintf(result, resultsize, "%s/%s", dir, fname);
	else
		snprintf(result, resultsize, "%s/%s/%s", DataDir, dir, fname);
}

/*
 * ──────────────────────────────────────────────────────────────────────────
 * Log entry formatter
 *
 * Builds a pt-query-digest-compatible slow log entry into the provided
 * StringInfo buffer.  The format mirrors the MySQL slow query log so that
 * pt-query-digest --type slowlog can parse it directly.
 *
 * Minimum output (all verbosity levels):
 *   # Time: <ISO-8601>
 *   # User@Host: user[user] @ host []
 *   # Query_time: N.NNNNNN  Lock_time: 0.000000  Rows_sent: N  Rows_examined: N
 *   SET timestamp=N;
 *   <query text>;
 *
 * Standard verbosity adds: Thread_id, Schema, Rows_affected, Bytes_sent
 * Full verbosity adds all extended metrics (handled in Phase 3).
 * ──────────────────────────────────────────────────────────────────────────
 */
static void
peql_format_entry(StringInfo buf, QueryDesc *queryDesc, double duration_ms)
{
	TimestampTz	now;
	pg_time_t	stamp_time;
	char		timebuf[128];
	struct pg_tm *tm_info;
	int			usec;

	const char *user  = NULL;
	const char *host  = NULL;
	const char *db    = NULL;
	uint64		rows_processed;
	double		duration_sec;
	const char *query_text;

	/* ---- Gather connection metadata ---- */
	if (MyProcPort)
	{
		user = MyProcPort->user_name;
		host = MyProcPort->remote_host;
		db   = MyProcPort->database_name;
	}
	if (user == NULL) user = "";
	if (host == NULL) host = "";
	if (db == NULL)   db   = "";

	rows_processed = queryDesc->estate->es_processed;
	duration_sec   = duration_ms / 1000.0;

	/* Use the original source text; fall back to an empty string. */
	query_text = queryDesc->sourceText ? queryDesc->sourceText : "";

	/*
	 * ---- # Time: line ----
	 *
	 * ISO-8601 timestamp with microseconds, matching the format that
	 * pt-query-digest recognises: YYYY-MM-DDTHH:MM:SS.uuuuuuZ
	 */
	now = GetCurrentTimestamp();
	stamp_time = timestamptz_to_time_t(now);
	tm_info = pg_localtime(&stamp_time, log_timezone);

	/* Extract microseconds from the TimestampTz (PG epoch microseconds). */
	usec = (int)(now % 1000000);
	if (usec < 0) usec += 1000000;

	snprintf(timebuf, sizeof(timebuf),
			 "%04d-%02d-%02dT%02d:%02d:%02d.%06d",
			 tm_info->tm_year + 1900,
			 tm_info->tm_mon + 1,
			 tm_info->tm_mday,
			 tm_info->tm_hour,
			 tm_info->tm_min,
			 tm_info->tm_sec,
			 usec);

	appendStringInfo(buf, "# Time: %s\n", timebuf);

	/*
	 * ---- # User@Host: line ----
	 *
	 * Format: user[user] @ host []
	 * The bracketed repeat of user mirrors MySQL slow log convention.
	 */
	appendStringInfo(buf, "# User@Host: %s[%s] @ %s []\n", user, user, host);

	/*
	 * ---- Standard verbosity: Thread_id, Schema ----
	 *
	 * Thread_id maps to the PostgreSQL backend PID.
	 * Schema is the database name (schema-level detail added in Phase 3).
	 */
	if (peql_log_verbosity >= PEQL_LOG_VERBOSITY_STANDARD)
	{
		appendStringInfo(buf, "# Thread_id: %d  Schema: %s\n",
						 MyProcPid, db);
	}

	/*
	 * ---- # Query_time / Lock_time / Rows_sent / Rows_examined ----
	 *
	 * This is the core line that pt-query-digest requires.
	 *  - Query_time: total execution time in seconds (float)
	 *  - Lock_time:  placeholder 0 for now (proper lock-wait time in Phase 3)
	 *  - Rows_sent:  for SELECT, the number of rows returned to the client
	 *  - Rows_examined: for now, same as Rows_sent (plan-tree walk in Phase 3)
	 *
	 * For DML (INSERT/UPDATE/DELETE), Rows_sent = 0 and Rows_examined = 0;
	 * the affected count goes to Rows_affected on the standard line.
	 */
	if (queryDesc->operation == CMD_SELECT)
	{
		appendStringInfo(buf,
						 "# Query_time: %f  Lock_time: 0.000000"
						 "  Rows_sent: " UINT64_FORMAT
						 "  Rows_examined: " UINT64_FORMAT "\n",
						 duration_sec,
						 rows_processed,
						 rows_processed);
	}
	else
	{
		appendStringInfo(buf,
						 "# Query_time: %f  Lock_time: 0.000000"
						 "  Rows_sent: 0  Rows_examined: 0\n",
						 duration_sec);
	}

	/* Rows_affected for DML at standard+ verbosity. */
	if (peql_log_verbosity >= PEQL_LOG_VERBOSITY_STANDARD &&
		queryDesc->operation != CMD_SELECT)
	{
		appendStringInfo(buf, "# Rows_affected: " UINT64_FORMAT "\n",
						 rows_processed);
	}

	/*
	 * ---- SET timestamp line ----
	 *
	 * Required by pt-query-digest to associate a UNIX timestamp with
	 * each entry.  We use the current time (end-of-query).
	 */
	appendStringInfo(buf, "SET timestamp=%ld;\n", (long) stamp_time);

	/*
	 * ---- Query text ----
	 *
	 * Terminated by a semicolon and a newline, which is how pt-query-digest
	 * delimits the end of one query entry.
	 */
	appendStringInfoString(buf, query_text);

	/* Ensure the query text ends with a semicolon. */
	if (buf->len > 0 && buf->data[buf->len - 1] != ';')
		appendStringInfoChar(buf, ';');
	appendStringInfoChar(buf, '\n');
}

/*
 * ──────────────────────────────────────────────────────────────────────────
 * File writer
 *
 * Appends pre-formatted data to the slow query log file.  Uses
 * AllocateFile/FreeFile so PostgreSQL tracks the file descriptor and
 * cleans up on transaction abort.
 *
 * Each call opens the file, writes, and closes.  This is safe for
 * concurrent backends because O_APPEND ensures atomic writes on POSIX
 * when the data fits in PIPE_BUF (typically 4-64 KB, well above a
 * single log entry).
 * ──────────────────────────────────────────────────────────────────────────
 */
static void
peql_flush_to_file(const char *data, int len)
{
	char	logpath[MAXPGPATH];
	char	dirpath[MAXPGPATH];
	char   *slash;
	FILE   *fp;

	peql_resolve_log_path(logpath, sizeof(logpath));

	fp = AllocateFile(logpath, "a");
	if (fp == NULL)
	{
		/*
		 * If the directory doesn't exist, try to create it and retry.
		 * This mirrors PostgreSQL's own syslogger behaviour.
		 */
		strlcpy(dirpath, logpath, sizeof(dirpath));

		/* Trim the filename component to get the directory. */
		slash = strrchr(dirpath, '/');
		if (slash)
			*slash = '\0';

		(void) MakePGDirectory(dirpath);

		fp = AllocateFile(logpath, "a");
		if (fp == NULL)
		{
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("peql: could not open log file \"%s\": %m",
							logpath)));
			return;
		}
	}

	(void) fwrite(data, 1, len, fp);
	FreeFile(fp);
}

/*
 * ──────────────────────────────────────────────────────────────────────────
 * Top-level log entry writer
 *
 * Called from ExecutorEnd when a query exceeds the duration threshold.
 * Formats the entry and flushes it to the log file.
 * ──────────────────────────────────────────────────────────────────────────
 */
static void
peql_write_log_entry(QueryDesc *queryDesc, double duration_ms)
{
	StringInfoData buf;

	initStringInfo(&buf);
	peql_format_entry(&buf, queryDesc, duration_ms);
	peql_flush_to_file(buf.data, buf.len);
	pfree(buf.data);
}

/*
 * ──────────────────────────────────────────────────────────────────────────
 * SQL-callable functions
 * ──────────────────────────────────────────────────────────────────────────
 */

/*
 * pg_enhanced_query_logging_reset()
 *
 * Truncates the slow query log file.  Returns true on success.
 */
Datum
pg_enhanced_query_logging_reset(PG_FUNCTION_ARGS)
{
	char	logpath[MAXPGPATH];
	FILE   *fp;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to reset the enhanced query log")));

	peql_resolve_log_path(logpath, sizeof(logpath));

	/*
	 * Open with "w" (truncate) rather than "a" (append).  If the file
	 * doesn't exist yet this creates it, which is fine.
	 */
	fp = AllocateFile(logpath, "w");
	if (fp == NULL)
	{
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("peql: could not truncate log file \"%s\": %m",
						logpath)));
		PG_RETURN_BOOL(false);
	}

	FreeFile(fp);

	ereport(NOTICE,
			(errmsg("peql: log file \"%s\" has been truncated", logpath)));

	PG_RETURN_BOOL(true);
}
