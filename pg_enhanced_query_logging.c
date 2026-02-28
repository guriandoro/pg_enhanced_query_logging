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

#include "catalog/namespace.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "commands/explain_state.h"
#include "common/pg_prng.h"
#include "executor/executor.h"
#include "executor/instrument.h"
#include "fmgr.h"
#include "jit/jit.h"
#include "lib/stringinfo.h"
#include "libpq/libpq-be.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "nodes/params.h"
#include "optimizer/planner.h"
#include "pgtime.h"
#include "postmaster/syslogger.h"
#include "storage/fd.h"
#include "tcop/utility.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"
#include "utils/tuplesort.h"

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

/* Rate limiting: log every Nth query or session (1 = log all). */
static int peql_rate_limit = 1;

/* Rate limiting mode. */
typedef enum
{
	PEQL_RATE_LIMIT_SESSION = 0,
	PEQL_RATE_LIMIT_QUERY
} PeqlRateLimitType;

static const struct config_enum_entry rate_limit_type_options[] = {
	{"session", PEQL_RATE_LIMIT_SESSION, false},
	{"query", PEQL_RATE_LIMIT_QUERY, false},
	{NULL, 0, false}
};

static int peql_rate_limit_type = PEQL_RATE_LIMIT_QUERY;

/*
 * Duration threshold (ms) that always bypasses the rate limiter.
 * Queries slower than this are always logged regardless of sampling.
 */
static int peql_rate_limit_always_log_duration = 10000;

/* Log bind parameter values in the log entry. */
static bool peql_log_parameter_values = false;

/* Include EXPLAIN output in the log entry. */
static bool peql_log_query_plan = false;

/* EXPLAIN output format. */
static const struct config_enum_entry query_plan_format_options[] = {
	{"text", EXPLAIN_FORMAT_TEXT, false},
	{"json", EXPLAIN_FORMAT_JSON, false},
	{NULL, 0, false}
};

static int peql_log_query_plan_format = EXPLAIN_FORMAT_TEXT;

/* Track block read/write times (requires PostgreSQL track_io_timing). */
static bool peql_track_io_timing = true;

/* Track WAL usage per query. */
static bool peql_track_wal = true;

/* Track memory context usage (experimental -- adds overhead). */
static bool peql_track_memory = false;

/* Track planning time separately from execution time. */
static bool peql_track_planning = false;

/* ---------- Rate limiting state ----------------------------------------- */

/*
 * Session-mode rate limiting: decided once at backend startup.
 * peql_session_is_sampled is true if this backend was selected for logging.
 * Initialised on first use via peql_session_decided flag.
 */
static bool peql_session_decided = false;
static bool peql_session_is_sampled = false;

/* ---------- Per-query planning time (set by planner hook) ---------------- */
static double peql_current_plan_time_ms = 0.0;

/*
 * Context passed through the plan-tree walker to collect plan quality
 * indicators (Full_scan, Filesort, Temp_table) and accurate Rows_examined.
 */
typedef struct PeqlPlanMetrics
{
	bool	has_seqscan;
	bool	has_sort;
	bool	has_sort_on_disk;
	bool	has_temp_table;
	bool	has_temp_on_disk;
	double	rows_examined;
} PeqlPlanMetrics;

/* ---------- Hook infrastructure ------------------------------------------ */

/* Nesting depth -- incremented around executor and utility calls. */
static int nesting_level = 0;

/* Separate nesting depth for the planner hook to avoid conflation. */
static int planner_nesting_level = 0;

/*
 * Convenience macro: the extension is "active" when it is enabled, the
 * duration threshold is set (>= 0), and we are either at the top-level
 * statement or nested-statement logging is on.
 */
#define peql_active() \
	(peql_enabled && peql_log_min_duration >= 0 && \
	 (nesting_level == 0 || peql_log_nested))

/* Saved previous hook values so we can chain properly. */
static planner_hook_type            prev_planner         = NULL;
static ExecutorStart_hook_type      prev_ExecutorStart   = NULL;
static ExecutorRun_hook_type        prev_ExecutorRun     = NULL;
static ExecutorFinish_hook_type     prev_ExecutorFinish  = NULL;
static ExecutorEnd_hook_type        prev_ExecutorEnd     = NULL;
static ProcessUtility_hook_type     prev_ProcessUtility  = NULL;

/* Forward declarations for hook functions. */
static PlannedStmt *peql_planner(Query *parse, const char *query_string,
								 int cursorOptions,
								 ParamListInfo boundParams);
static void peql_ExecutorStart(QueryDesc *queryDesc, int eflags);
static void peql_ExecutorRun(QueryDesc *queryDesc,
							 ScanDirection direction,
							 uint64 count);
static void peql_ExecutorFinish(QueryDesc *queryDesc);
static void peql_ExecutorEnd(QueryDesc *queryDesc);
static void peql_ProcessUtility(PlannedStmt *pstmt,
								const char *queryString,
								bool readOnlyTree,
								ProcessUtilityContext context,
								ParamListInfo params,
								QueryEnvironment *queryEnv,
								DestReceiver *dest,
								QueryCompletion *qc);

/* Forward declarations for log formatting and file I/O. */
static void peql_write_log_entry(QueryDesc *queryDesc, double duration_ms);
static void peql_write_utility_log_entry(const char *queryString,
										 double duration_ms,
										 ParamListInfo params);
static void peql_resolve_log_path(char *result, size_t resultsize);
static void peql_format_entry(StringInfo buf, QueryDesc *queryDesc,
							  double duration_ms);
static void peql_format_utility_entry(StringInfo buf,
									  const char *queryString,
									  double duration_ms,
									  ParamListInfo params);
static void peql_flush_to_file(const char *data, int len);

/* Rate limiting. */
static bool peql_should_log(double duration_ms);

/* Plan-tree analysis. */
static bool peql_plan_walker(PlanState *planstate, void *context);
static void peql_collect_plan_metrics(QueryDesc *queryDesc,
									  PeqlPlanMetrics *metrics);

/* Parameter formatting helper. */
static void peql_append_params(StringInfo buf, ParamListInfo params);

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

	/* ---- GUC: peql.rate_limit ---- */
	DefineCustomIntVariable("peql.rate_limit",
							"Log every Nth query or session.",
							"1 means log all eligible queries.  Higher values "
							"sample 1-in-N.  Works with peql.rate_limit_type.",
							&peql_rate_limit,
							1,
							1, INT_MAX,
							PGC_SUSET,
							0,
							NULL, NULL, NULL);

	/* ---- GUC: peql.rate_limit_type ---- */
	DefineCustomEnumVariable("peql.rate_limit_type",
							 "Rate limiting mode: session or query.",
							 "'session' decides once per backend whether to "
							 "log; 'query' decides per individual query.",
							 &peql_rate_limit_type,
							 PEQL_RATE_LIMIT_QUERY,
							 rate_limit_type_options,
							 PGC_SUSET,
							 0,
							 NULL, NULL, NULL);

	/* ---- GUC: peql.rate_limit_always_log_duration ---- */
	DefineCustomIntVariable("peql.rate_limit_always_log_duration",
							"Duration (ms) that always bypasses the rate limiter.",
							"Queries exceeding this threshold are logged "
							"regardless of sampling.  -1 = disabled, "
							"0 = bypass for all queries, 10000 = 10 seconds.",
							&peql_rate_limit_always_log_duration,
							10000,
							-1, INT_MAX,
							PGC_SUSET,
							GUC_UNIT_MS,
							NULL, NULL, NULL);

	/* ---- GUC: peql.log_parameter_values ---- */
	DefineCustomBoolVariable("peql.log_parameter_values",
							 "Log bind parameter values.",
							 "When enabled, parameter values are appended "
							 "after the query text in the log entry.",
							 &peql_log_parameter_values,
							 false,
							 PGC_SUSET,
							 0,
							 NULL, NULL, NULL);

	/* ---- GUC: peql.log_query_plan ---- */
	DefineCustomBoolVariable("peql.log_query_plan",
							 "Include EXPLAIN output in log entry.",
							 "Appends the query execution plan after the "
							 "query text.  Uses EXPLAIN ANALYZE (actual rows "
							 "and timing from the just-finished execution).",
							 &peql_log_query_plan,
							 false,
							 PGC_SUSET,
							 0,
							 NULL, NULL, NULL);

	/* ---- GUC: peql.log_query_plan_format ---- */
	DefineCustomEnumVariable("peql.log_query_plan_format",
							 "EXPLAIN format for log_query_plan output.",
							 NULL,
							 &peql_log_query_plan_format,
							 EXPLAIN_FORMAT_TEXT,
							 query_plan_format_options,
							 PGC_SUSET,
							 0,
							 NULL, NULL, NULL);

	/* ---- GUC: peql.track_io_timing ---- */
	DefineCustomBoolVariable("peql.track_io_timing",
							 "Include block read/write times in log output.",
							 "Requires PostgreSQL's track_io_timing to be on "
							 "for the underlying counters to be populated.",
							 &peql_track_io_timing,
							 true,
							 PGC_SUSET,
							 0,
							 NULL, NULL, NULL);

	/* ---- GUC: peql.track_wal ---- */
	DefineCustomBoolVariable("peql.track_wal",
							 "Include WAL usage metrics in log output.",
							 NULL,
							 &peql_track_wal,
							 true,
							 PGC_SUSET,
							 0,
							 NULL, NULL, NULL);

	/* ---- GUC: peql.track_memory ---- */
	DefineCustomBoolVariable("peql.track_memory",
							 "Include memory context allocation in log output.",
							 "Experimental. Adds some overhead to capture "
							 "the memory allocated by the query.",
							 &peql_track_memory,
							 false,
							 PGC_SUSET,
							 0,
							 NULL, NULL, NULL);

	/* ---- GUC: peql.track_planning ---- */
	DefineCustomBoolVariable("peql.track_planning",
							 "Track and log planning time separately.",
							 "Installs a planner hook to measure how long "
							 "the query planner takes.",
							 &peql_track_planning,
							 false,
							 PGC_SUSET,
							 0,
							 NULL, NULL, NULL);

	/* Reserve the "peql" GUC prefix so no other extension can claim it. */
	MarkGUCPrefixReserved("peql");

	/* ---- Install planner hook ---- */
	prev_planner       = planner_hook;
	planner_hook       = peql_planner;

	/* ---- Install executor hooks ---- */
	prev_ExecutorStart  = ExecutorStart_hook;
	ExecutorStart_hook  = peql_ExecutorStart;

	prev_ExecutorRun    = ExecutorRun_hook;
	ExecutorRun_hook    = peql_ExecutorRun;

	prev_ExecutorFinish = ExecutorFinish_hook;
	ExecutorFinish_hook = peql_ExecutorFinish;

	prev_ExecutorEnd    = ExecutorEnd_hook;
	ExecutorEnd_hook    = peql_ExecutorEnd;

	/* ---- Install ProcessUtility hook (for DDL/utility logging) ---- */
	prev_ProcessUtility  = ProcessUtility_hook;
	ProcessUtility_hook  = peql_ProcessUtility;
}

/*
 * ──────────────────────────────────────────────────────────────────────────
 * Planner hook
 *
 * When peql.track_planning is enabled, wraps the standard planner to
 * measure planning wall-clock time.  The result is stored in a static
 * variable and consumed by ExecutorEnd when writing the log entry.
 *
 * The hook is always installed (to avoid needing a restart to toggle
 * peql.track_planning), but it only takes the measurement when the
 * GUC is on and the extension is active.
 * ──────────────────────────────────────────────────────────────────────────
 */
static PlannedStmt *
peql_planner(Query *parse, const char *query_string,
			 int cursorOptions, ParamListInfo boundParams)
{
	PlannedStmt *result;
	instr_time	start;
	instr_time	duration;

	if (!peql_track_planning || !peql_enabled || peql_log_min_duration < 0)
	{
		if (prev_planner)
			return prev_planner(parse, query_string, cursorOptions,
								boundParams);
		else
			return standard_planner(parse, query_string, cursorOptions,
									boundParams);
	}

	INSTR_TIME_SET_CURRENT(start);

	planner_nesting_level++;
	PG_TRY();
	{
		if (prev_planner)
			result = prev_planner(parse, query_string, cursorOptions,
								  boundParams);
		else
			result = standard_planner(parse, query_string, cursorOptions,
									  boundParams);
	}
	PG_FINALLY();
	{
		planner_nesting_level--;
	}
	PG_END_TRY();

	INSTR_TIME_SET_CURRENT(duration);
	INSTR_TIME_SUBTRACT(duration, start);
	peql_current_plan_time_ms = INSTR_TIME_GET_MILLISEC(duration);

	return result;
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
	/*
	 * Enable per-node instrumentation before the executor builds the plan
	 * state tree.  ExecInitNode reads instrument_options to decide whether
	 * to attach Instrumentation structs to each node; we need those for
	 * Rows_examined, sort-spill detection, and other plan-tree metrics.
	 *
	 * At full verbosity we request INSTRUMENT_ALL (timing + buffers); at
	 * lower levels INSTRUMENT_TIMER suffices for the top-level totaltime.
	 */
	if (peql_active())
	{
		if (peql_log_verbosity >= PEQL_LOG_VERBOSITY_FULL)
			queryDesc->instrument_options |= INSTRUMENT_ALL;
		else
			queryDesc->instrument_options |= INSTRUMENT_TIMER;
	}

	/* Chain to any previously-installed hook, or the standard function. */
	if (prev_ExecutorStart)
		prev_ExecutorStart(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);

	/*
	 * Ensure the top-level totaltime counter exists so ExecutorEnd can
	 * read overall execution duration regardless of verbosity level.
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
 * Rate limiter
 *
 * Decides whether the current query should be logged, taking into account
 * the sampling rate and the always-log-duration override.
 *
 * Session mode: The first time this backend calls us, we draw once from
 *   the PRNG; if selected, every query in this session is logged.
 * Query mode: Each invocation draws independently.
 *
 * Queries that exceed peql.rate_limit_always_log_duration bypass the
 * sampler entirely and are always logged.
 * ──────────────────────────────────────────────────────────────────────────
 */
static bool
peql_should_log(double duration_ms)
{
	/* Always-log override: very slow queries bypass the rate limiter. (-1 = disabled) */
	if (peql_rate_limit_always_log_duration >= 0 &&
		duration_ms >= peql_rate_limit_always_log_duration)
		return true;

	/* No rate limiting when rate_limit == 1 (log everything). */
	if (peql_rate_limit <= 1)
		return true;

	if (peql_rate_limit_type == PEQL_RATE_LIMIT_SESSION)
	{
		if (!peql_session_decided)
		{
			uint32 r = (uint32) (pg_prng_uint64(&pg_global_prng_state)
								 % (uint64) peql_rate_limit);
			peql_session_is_sampled = (r == 0);
			peql_session_decided = true;
		}
		return peql_session_is_sampled;
	}

	/* PEQL_RATE_LIMIT_QUERY: per-query draw. */
	{
		uint32 r = (uint32) (pg_prng_uint64(&pg_global_prng_state)
							 % (uint64) peql_rate_limit);
		return (r == 0);
	}
}

/*
 * ──────────────────────────────────────────────────────────────────────────
 * ExecutorEnd hook
 *
 * After the query has finished executing, check whether its duration
 * exceeds the configured threshold and, if so, apply the rate limiter
 * and write a log entry.
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

		if (msec >= peql_log_min_duration && peql_should_log(msec))
			peql_write_log_entry(queryDesc, msec);
	}

	/* Always reset so plan time doesn't leak to the next query. */
	peql_current_plan_time_ms = 0.0;

	/* Chain to previous hook or the standard cleanup. */
	if (prev_ExecutorEnd)
		prev_ExecutorEnd(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);
}

/*
 * ──────────────────────────────────────────────────────────────────────────
 * ProcessUtility hook
 *
 * Logs utility (DDL) statements when peql.log_utility is enabled.  Uses
 * the same duration filter and rate limiter as regular queries.
 * ──────────────────────────────────────────────────────────────────────────
 */
static void
peql_ProcessUtility(PlannedStmt *pstmt,
					const char *queryString,
					bool readOnlyTree,
					ProcessUtilityContext context,
					ParamListInfo params,
					QueryEnvironment *queryEnv,
					DestReceiver *dest,
					QueryCompletion *qc)
{
	instr_time	start;
	bool		do_log;

	do_log = peql_log_utility && peql_enabled && peql_log_min_duration >= 0 &&
		(nesting_level == 0 || peql_log_nested);

	if (do_log)
		INSTR_TIME_SET_CURRENT(start);

	nesting_level++;
	PG_TRY();
	{
		if (prev_ProcessUtility)
			prev_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);
		else
			standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
									params, queryEnv, dest, qc);
	}
	PG_FINALLY();
	{
		nesting_level--;
	}
	PG_END_TRY();

	if (do_log)
	{
		instr_time	duration;
		double		msec;

		/* Re-check in case the utility statement changed our GUCs. */
		do_log = peql_log_utility && peql_enabled && peql_log_min_duration >= 0;
		if (!do_log)
			return;

		INSTR_TIME_SET_CURRENT(duration);
		INSTR_TIME_SUBTRACT(duration, start);
		msec = INSTR_TIME_GET_MILLISEC(duration);

		if (msec >= peql_log_min_duration && peql_should_log(msec))
			peql_write_utility_log_entry(queryString, msec, params);
	}
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
 * Plan-tree walker
 *
 * Walks the executed plan tree to extract plan quality indicators:
 *   - Full_scan: any SeqScan node present
 *   - Filesort / Filesort_on_disk: Sort nodes, and whether they spilled
 *   - Temp_table / Temp_table_on_disk: Material nodes with disk spill
 *   - Rows_examined: sum of ntuples across all scan nodes
 *
 * The walker uses planstate_tree_walker so it visits every node in the
 * tree, including subplans and append children.
 * ──────────────────────────────────────────────────────────────────────────
 */
static bool
peql_plan_walker(PlanState *planstate, void *context)
{
	PeqlPlanMetrics *m = (PeqlPlanMetrics *) context;

	if (planstate == NULL)
		return false;

	/*
	 * Accumulate rows from instrumented scan nodes.  We read tuplecount
	 * (the running counter) + ntuples (already-finalised loops) because
	 * InstrEndLoop has not been called on per-node instrumentation at the
	 * point we walk the tree -- only queryDesc->totaltime gets finalised.
	 */
	if (planstate->instrument)
		m->rows_examined += planstate->instrument->ntuples
						  + planstate->instrument->tuplecount;

	if (IsA(planstate, SeqScanState))
		m->has_seqscan = true;

	if (IsA(planstate, SortState))
	{
		SortState  *ss = (SortState *) planstate;

		m->has_sort = true;

		if (ss->sort_Done && ss->tuplesortstate != NULL)
		{
			TuplesortInstrumentation stats;

			tuplesort_get_stats((Tuplesortstate *) ss->tuplesortstate, &stats);
			if (stats.spaceType == SORT_SPACE_TYPE_DISK)
				m->has_sort_on_disk = true;
		}
	}

	if (IsA(planstate, MaterialState))
	{
		m->has_temp_table = true;
		/* Temp_table_on_disk is inferred from temp_blks_written > 0. */
	}

	return planstate_tree_walker(planstate, peql_plan_walker, context);
}

/*
 * Convenience wrapper that initialises the metrics struct and kicks off
 * the walk.  Safe to call even when the planstate is NULL (returns zeros).
 */
static void
peql_collect_plan_metrics(QueryDesc *queryDesc, PeqlPlanMetrics *metrics)
{
	memset(metrics, 0, sizeof(PeqlPlanMetrics));

	if (queryDesc->planstate)
		(void) peql_plan_walker(queryDesc->planstate, metrics);
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
 * Standard verbosity adds: Thread_id, Schema (db.schema), Rows_affected,
 *   Bytes_sent
 *
 * Full verbosity adds: buffer/WAL/JIT metrics, plan quality booleans,
 *   planning time, memory usage
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

	Instrumentation *instr;
	PeqlPlanMetrics	plan_metrics;
	bool			have_plan_metrics = false;

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

	instr = queryDesc->totaltime;
	rows_processed = queryDesc->estate->es_processed;
	duration_sec   = duration_ms / 1000.0;
	query_text = queryDesc->sourceText ? queryDesc->sourceText : "";

	/*
	 * Collect plan-tree metrics when full verbosity is enabled.  We do
	 * this before finalizing the format so the data is available.
	 */
	if (peql_log_verbosity >= PEQL_LOG_VERBOSITY_FULL && queryDesc->planstate)
	{
		peql_collect_plan_metrics(queryDesc, &plan_metrics);
		have_plan_metrics = true;
	}

	/* ---- # Time: line ---- */
	now = GetCurrentTimestamp();
	stamp_time = timestamptz_to_time_t(now);
	tm_info = pg_localtime(&stamp_time, log_timezone);

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

	/* ---- # User@Host: line ---- */
	appendStringInfo(buf, "# User@Host: %s[%s] @ %s []\n", user, user, host);

	/*
	 * ---- Standard verbosity: Thread_id, Schema (db.schema) ----
	 *
	 * Schema now uses "dbname.schemaname" format, with the current
	 * schema obtained from the search path.
	 */
	if (peql_log_verbosity >= PEQL_LOG_VERBOSITY_STANDARD)
	{
		const char *schema_name = NULL;
		List	   *search_path;

		search_path = fetch_search_path(false);
		if (search_path != NIL)
		{
			Oid		first_ns = linitial_oid(search_path);

			schema_name = get_namespace_name(first_ns);
			list_free(search_path);
		}

		if (schema_name)
			appendStringInfo(buf, "# Thread_id: %d  Schema: %s.%s\n",
							 MyProcPid, db, schema_name);
		else
			appendStringInfo(buf, "# Thread_id: %d  Schema: %s\n",
							 MyProcPid, db);
	}

	/*
	 * ---- # Query_time / Lock_time / Rows_sent / Rows_examined ----
	 *
	 * Core line required by pt-query-digest.  When we have plan metrics
	 * from the tree walk, Rows_examined comes from actual scan-node
	 * ntuples counts rather than being a copy of Rows_sent.
	 */
	if (queryDesc->operation == CMD_SELECT)
	{
		double examined = have_plan_metrics
			? plan_metrics.rows_examined
			: (double) rows_processed;

		appendStringInfo(buf,
						 "# Query_time: %f  Lock_time: 0.000000"
						 "  Rows_sent: " UINT64_FORMAT
						 "  Rows_examined: %.0f\n",
						 duration_sec,
						 rows_processed,
						 examined);
	}
	else
	{
		double examined = have_plan_metrics
			? plan_metrics.rows_examined : 0.0;

		appendStringInfo(buf,
						 "# Query_time: %f  Lock_time: 0.000000"
						 "  Rows_sent: 0  Rows_examined: %.0f\n",
						 duration_sec,
						 examined);
	}

	/* Rows_affected for DML at standard+ verbosity. */
	if (peql_log_verbosity >= PEQL_LOG_VERBOSITY_STANDARD &&
		queryDesc->operation != CMD_SELECT)
	{
		appendStringInfo(buf, "# Rows_affected: " UINT64_FORMAT "\n",
						 rows_processed);
	}

	/*
	 * ────────────────────────────────────────────────────────
	 * Full verbosity: extended metrics
	 * ────────────────────────────────────────────────────────
	 */
	if (peql_log_verbosity >= PEQL_LOG_VERBOSITY_FULL && instr)
	{
		BufferUsage *bu = &instr->bufusage;
		WalUsage	*wu = &instr->walusage;

		/* ---- Buffer usage ---- */
		appendStringInfo(buf,
						 "# Shared_blks_hit: " INT64_FORMAT
						 "  Shared_blks_read: " INT64_FORMAT
						 "  Shared_blks_dirtied: " INT64_FORMAT
						 "  Shared_blks_written: " INT64_FORMAT "\n",
						 bu->shared_blks_hit,
						 bu->shared_blks_read,
						 bu->shared_blks_dirtied,
						 bu->shared_blks_written);

		appendStringInfo(buf,
						 "# Local_blks_hit: " INT64_FORMAT
						 "  Local_blks_read: " INT64_FORMAT
						 "  Local_blks_dirtied: " INT64_FORMAT
						 "  Local_blks_written: " INT64_FORMAT "\n",
						 bu->local_blks_hit,
						 bu->local_blks_read,
						 bu->local_blks_dirtied,
						 bu->local_blks_written);

		appendStringInfo(buf,
						 "# Temp_blks_read: " INT64_FORMAT
						 "  Temp_blks_written: " INT64_FORMAT "\n",
						 bu->temp_blks_read,
						 bu->temp_blks_written);

		/* ---- I/O timing (block read/write times) ---- */
		if (peql_track_io_timing)
		{
			appendStringInfo(buf,
							 "# Shared_blk_read_time: %f"
							 "  Shared_blk_write_time: %f\n",
							 INSTR_TIME_GET_MILLISEC(bu->shared_blk_read_time) / 1000.0,
							 INSTR_TIME_GET_MILLISEC(bu->shared_blk_write_time) / 1000.0);
		}

		/* ---- WAL usage ---- */
		if (peql_track_wal)
		{
			appendStringInfo(buf,
							 "# WAL_records: " INT64_FORMAT
							 "  WAL_bytes: " UINT64_FORMAT
							 "  WAL_fpi: " INT64_FORMAT "\n",
							 wu->wal_records,
							 wu->wal_bytes,
							 wu->wal_fpi);
		}

		/* ---- Planning time ---- */
		if (peql_track_planning && peql_current_plan_time_ms > 0)
		{
			appendStringInfo(buf, "# Plan_time: %f\n",
							 peql_current_plan_time_ms / 1000.0);
		}

		/* ---- Plan quality indicators ---- */
		if (have_plan_metrics)
		{
			appendStringInfo(buf,
							 "# Full_scan: %s"
							 "  Temp_table: %s"
							 "  Temp_table_on_disk: %s"
							 "  Filesort: %s"
							 "  Filesort_on_disk: %s\n",
							 plan_metrics.has_seqscan ? "Yes" : "No",
							 plan_metrics.has_temp_table ? "Yes" : "No",
							 (bu->temp_blks_written > 0) ? "Yes" : "No",
							 plan_metrics.has_sort ? "Yes" : "No",
							 plan_metrics.has_sort_on_disk ? "Yes" : "No");
		}

		/* ---- JIT metrics ---- */
		if (queryDesc->estate->es_jit)
		{
			JitInstrumentation ji = {0};

			InstrJitAgg(&ji, &queryDesc->estate->es_jit->instr);

			if (queryDesc->estate->es_jit_worker_instr)
				InstrJitAgg(&ji, queryDesc->estate->es_jit_worker_instr);

			if (ji.created_functions > 0)
			{
				appendStringInfo(buf,
								 "# JIT_functions: %zu"
								 "  JIT_generation_time: %f"
								 "  JIT_inlining_time: %f"
								 "  JIT_optimization_time: %f"
								 "  JIT_emission_time: %f\n",
								 ji.created_functions,
								 INSTR_TIME_GET_MILLISEC(ji.generation_counter) / 1000.0,
								 INSTR_TIME_GET_MILLISEC(ji.inlining_counter) / 1000.0,
								 INSTR_TIME_GET_MILLISEC(ji.optimization_counter) / 1000.0,
								 INSTR_TIME_GET_MILLISEC(ji.emission_counter) / 1000.0);
			}
		}

		/* ---- Memory context usage ---- */
		if (peql_track_memory && queryDesc->estate->es_query_cxt)
		{
			Size mem = MemoryContextMemAllocated(
						queryDesc->estate->es_query_cxt, true);

			appendStringInfo(buf, "# Mem_allocated: %zu\n", mem);
		}
	}

	/* ---- Rate limit metadata (all verbosity levels when rate_limit > 1) ---- */
	if (peql_rate_limit > 1)
	{
		appendStringInfo(buf,
						 "# Log_slow_rate_type: %s  Log_slow_rate_limit: %d\n",
						 (peql_rate_limit_type == PEQL_RATE_LIMIT_SESSION)
						 ? "session" : "query",
						 peql_rate_limit);
	}

	/* ---- SET timestamp line (query start time, not completion time) ---- */
	appendStringInfo(buf, "SET timestamp=" INT64_FORMAT ";\n",
					 (int64) stamp_time - (int64)(duration_ms / 1000.0));

	/* ---- Query text ---- */
	appendStringInfoString(buf, query_text);

	if (buf->len > 0 && buf->data[buf->len - 1] != ';')
		appendStringInfoChar(buf, ';');
	appendStringInfoChar(buf, '\n');

	/* ---- Bind parameter values ---- */
	if (peql_log_parameter_values && queryDesc->params)
		peql_append_params(buf, queryDesc->params);

	/* ---- EXPLAIN plan output ---- */
	if (peql_log_query_plan && queryDesc->planstate)
	{
		ExplainState *es = NewExplainState();

		es->analyze = true;
		es->verbose = false;
		es->buffers = (peql_log_verbosity >= PEQL_LOG_VERBOSITY_FULL);
		es->wal     = (peql_track_wal &&
					   peql_log_verbosity >= PEQL_LOG_VERBOSITY_FULL);
		es->timing  = true;
		es->summary = false;
		es->format  = peql_log_query_plan_format;

		ExplainBeginOutput(es);
		ExplainPrintPlan(es, queryDesc);
		if (es->costs)
			ExplainPrintJITSummary(es, queryDesc);
		ExplainEndOutput(es);

		/* Trim trailing newline from EXPLAIN output. */
		if (es->str->len > 0 && es->str->data[es->str->len - 1] == '\n')
			es->str->data[--es->str->len] = '\0';

		appendStringInfo(buf, "# Plan:\n# %s\n", es->str->data);
	}
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
 * Parameter value formatter
 *
 * Appends a "# Parameters: $1 = '...', $2 = '...'" line to the log entry.
 * Handles NULL values and uses the datum's text output function.
 * ──────────────────────────────────────────────────────────────────────────
 */
static void
peql_append_params(StringInfo buf, ParamListInfo params)
{
	int		i;
	int		nparams;
	bool	first = true;

	if (params == NULL)
		return;

	nparams = params->numParams;
	if (nparams <= 0)
		return;

	appendStringInfoString(buf, "# Parameters: ");

	for (i = 0; i < nparams; i++)
	{
		ParamExternData *prm = &params->params[i];

		if (!first)
			appendStringInfoString(buf, ", ");
		first = false;

		appendStringInfo(buf, "$%d = ", i + 1);

		if (prm->isnull || !OidIsValid(prm->ptype))
		{
			appendStringInfoString(buf, "NULL");
		}
		else
		{
			Oid		typoutput;
			bool	typisvarlena;
			char   *val;

			getTypeOutputInfo(prm->ptype, &typoutput, &typisvarlena);
			val = OidOutputFunctionCall(typoutput, prm->value);
			appendStringInfo(buf, "'%s'", val);
			pfree(val);
		}
	}

	appendStringInfoChar(buf, '\n');
}

/*
 * ──────────────────────────────────────────────────────────────────────────
 * Utility statement log entry formatter
 *
 * Similar to peql_format_entry but for DDL/utility statements which don't
 * go through the executor and therefore have no QueryDesc or plan tree.
 * ──────────────────────────────────────────────────────────────────────────
 */
static void
peql_format_utility_entry(StringInfo buf, const char *queryString,
						  double duration_ms, ParamListInfo params)
{
	TimestampTz	now;
	pg_time_t	stamp_time;
	char		timebuf[128];
	struct pg_tm *tm_info;
	int			usec;
	const char *user  = NULL;
	const char *host  = NULL;
	const char *db    = NULL;
	double		duration_sec;

	if (MyProcPort)
	{
		user = MyProcPort->user_name;
		host = MyProcPort->remote_host;
		db   = MyProcPort->database_name;
	}
	if (user == NULL) user = "";
	if (host == NULL) host = "";
	if (db == NULL)   db   = "";

	duration_sec = duration_ms / 1000.0;

	now = GetCurrentTimestamp();
	stamp_time = timestamptz_to_time_t(now);
	tm_info = pg_localtime(&stamp_time, log_timezone);

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
	appendStringInfo(buf, "# User@Host: %s[%s] @ %s []\n", user, user, host);

	if (peql_log_verbosity >= PEQL_LOG_VERBOSITY_STANDARD)
	{
		appendStringInfo(buf, "# Thread_id: %d  Schema: %s\n",
						 MyProcPid, db);
	}

	appendStringInfo(buf,
					 "# Query_time: %f  Lock_time: 0.000000"
					 "  Rows_sent: 0  Rows_examined: 0\n",
					 duration_sec);

	if (peql_rate_limit > 1)
	{
		appendStringInfo(buf,
						 "# Log_slow_rate_type: %s  Log_slow_rate_limit: %d\n",
						 (peql_rate_limit_type == PEQL_RATE_LIMIT_SESSION)
						 ? "session" : "query",
						 peql_rate_limit);
	}

	/* SET timestamp reflects query start time, not completion time. */
	appendStringInfo(buf, "SET timestamp=" INT64_FORMAT ";\n",
					 (int64) stamp_time - (int64)(duration_ms / 1000.0));
	appendStringInfoString(buf, queryString ? queryString : "");

	if (buf->len > 0 && buf->data[buf->len - 1] != ';')
		appendStringInfoChar(buf, ';');
	appendStringInfoChar(buf, '\n');

	if (peql_log_parameter_values && params)
		peql_append_params(buf, params);
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
 * Writer for utility (DDL) log entries.
 */
static void
peql_write_utility_log_entry(const char *queryString, double duration_ms,
							 ParamListInfo params)
{
	StringInfoData buf;

	initStringInfo(&buf);
	peql_format_utility_entry(&buf, queryString, duration_ms, params);
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
