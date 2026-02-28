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
#include <unistd.h>

#include "executor/executor.h"
#include "executor/instrument.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "utils/guc.h"

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

/* Forward declaration for the log-writing stub. */
static void peql_write_log_entry(QueryDesc *queryDesc, double duration_ms);

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
 * Log-writing stub
 *
 * Phase 1: emits a one-line notice via elog() to confirm the hook fires.
 * Phase 2 will replace this with the full pt-query-digest-compatible
 * formatter and file writer.
 * ──────────────────────────────────────────────────────────────────────────
 */
static void
peql_write_log_entry(QueryDesc *queryDesc, double duration_ms)
{
	elog(DEBUG1, "peql: query duration %.3f ms (would log to %s/%s)",
		 duration_ms,
		 (peql_log_directory && peql_log_directory[0] != '\0')
		 ? peql_log_directory : "<log_directory>",
		 peql_log_filename ? peql_log_filename : "peql-slow.log");
}

/*
 * ──────────────────────────────────────────────────────────────────────────
 * SQL-callable functions
 * ──────────────────────────────────────────────────────────────────────────
 */

/*
 * pg_enhanced_query_logging_reset()
 *
 * Truncates the slow query log file.  Stub implementation for Phase 1;
 * will perform actual file truncation once the file writer is in place.
 */
Datum
pg_enhanced_query_logging_reset(PG_FUNCTION_ARGS)
{
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to reset the enhanced query log")));

	elog(NOTICE, "peql: log reset requested (no-op until file writer is implemented)");

	PG_RETURN_BOOL(true);
}
