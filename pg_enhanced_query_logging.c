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
 * Copyright (c) 2025-2026, guriandoro
 *
 * IDENTIFICATION
 *   pg_enhanced_query_logging/pg_enhanced_query_logging.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef WIN32
#include <sys/statvfs.h>
#endif

#include "catalog/namespace.h"
#include "commands/explain.h"
#if PG_VERSION_NUM >= 180000
#include "commands/explain_format.h"
#include "commands/explain_state.h"
#endif
#include "common/file_perm.h"
#include "common/pg_prng.h"
#include "executor/executor.h"
#include "executor/instrument.h"
#include "fmgr.h"
#include "funcapi.h"
#include "jit/jit.h"
#include "lib/stringinfo.h"
#include "libpq/libpq-be.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "nodes/params.h"
#include "optimizer/planner.h"
#include "parser/analyze.h"
#include "parser/parsetree.h"
#include "pgtime.h"
#include "postmaster/syslogger.h"
#include "port/atomics.h"
#include "storage/fd.h"
#include "storage/proc.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "tcop/dest.h"
#include "tcop/utility.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/timeout.h"
#include "utils/timestamp.h"
#include "utils/tuplesort.h"
#include "utils/wait_event.h"

#if PG_VERSION_NUM >= 180000
PG_MODULE_MAGIC_EXT(
	.name = "pg_enhanced_query_logging",
	.version = "1.3"
);
#else
PG_MODULE_MAGIC;
#endif

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

/*
 * Adaptive rate limiting: global (shared-memory-backed) throttles that
 * limit how many queries or bytes are logged cluster-wide per second.
 * 0 = disabled for each limit independently.
 */
static int peql_rate_limit_auto_max_queries = 0;
static int peql_rate_limit_auto_max_bytes   = 0;

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

/* Track wait events via periodic sampling (experimental). */
static bool peql_track_wait_events = false;

/* Log at transaction boundary instead of per-statement. */
static bool peql_log_transaction = false;

/* Track planning time separately from execution time. */
static bool peql_track_planning = false;

/*
 * Disk space protection: pause logging when the log mountpoint runs low.
 * 0 = disabled; otherwise, free space percentage below which logging pauses.
 */
static int peql_disk_threshold_pct = 5;

/* Minimum interval (ms) between statvfs() calls to check disk space. */
static int peql_disk_check_interval_ms = 5000;

/* Automatically delete old rotated (.old) log files when disk is low. */
static bool peql_disk_auto_purge = false;

/* ---------- Rate limiting state ----------------------------------------- */

/*
 * Session-mode rate limiting: decided once at backend startup.
 * peql_session_is_sampled is true if this backend was selected for logging.
 * Initialised on first use via peql_session_decided flag.
 */
static bool peql_session_decided = false;
static bool peql_session_is_sampled = false;

/* ---------- Shared memory state for adaptive rate limiting --------------- */

typedef struct PeqlSharedState
{
	/* Adaptive rate limiting (per-window counters) */
	pg_atomic_uint64	queries_this_window;
	pg_atomic_uint64	bytes_this_window;
	pg_atomic_uint64	window_start_usec;
	/* Global statistics counters (across all backends) */
	pg_atomic_uint64	total_queries_logged;
	pg_atomic_uint64	total_queries_skipped;
	pg_atomic_uint64	total_bytes_written;
	/* Disk space protection */
	pg_atomic_uint32	disk_paused;
	pg_atomic_uint32	purge_in_progress;
	pg_atomic_uint64	total_disk_skipped;
	pg_atomic_uint64	last_disk_check_usec;
} PeqlSharedState;

static PeqlSharedState *peql_shared = NULL;

static shmem_request_hook_type prev_shmem_request_hook = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;

/* ---------- Per-query planning time (set by planner hook) ---------------- */
static double peql_current_plan_time_ms = 0.0;

/* ---------- Per-query ID (set by post_parse_analyze hook) --------------- */
static int64 peql_current_query_id = 0;

/* ---------- Wait event sampling state ----------------------------------- */

#define PEQL_MAX_WAIT_EVENTS	64

typedef struct PeqlWaitEventSample
{
	uint32	wait_event_info;
	int		count;
} PeqlWaitEventSample;

static PeqlWaitEventSample peql_wait_samples[PEQL_MAX_WAIT_EVENTS];
static int peql_wait_sample_count = 0;
static bool peql_wait_sampling_active = false;
static TimeoutId peql_wait_timeout_id = MAX_TIMEOUTS;

/* ---------- Transaction-aware logging accumulator ----------------------- */

typedef struct PeqlTxnAccumulator
{
	double			total_duration_ms;
	uint64			total_rows_sent;
	double			total_rows_examined;
	uint64			total_rows_affected;
	int				statement_count;
	StringInfoData	query_texts;
	bool			active;
} PeqlTxnAccumulator;

static PeqlTxnAccumulator peql_txn_accum;

/*
 * Per-table I/O attribution: tracks buffer hits and reads for each
 * table (relation OID) seen during a plan-tree walk.
 */
#define PEQL_MAX_TABLE_IO	32

typedef struct PeqlTableIO
{
	Oid		relid;
	int64	blks_hit;
	int64	blks_read;
} PeqlTableIO;

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
	EState *estate;
	PeqlTableIO	table_io[PEQL_MAX_TABLE_IO];
	int		table_io_count;
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
static post_parse_analyze_hook_type prev_post_parse_analyze = NULL;
static planner_hook_type            prev_planner         = NULL;
static ExecutorStart_hook_type      prev_ExecutorStart   = NULL;
static ExecutorRun_hook_type        prev_ExecutorRun     = NULL;
static ExecutorFinish_hook_type     prev_ExecutorFinish  = NULL;
static ExecutorEnd_hook_type        prev_ExecutorEnd     = NULL;
static ProcessUtility_hook_type     prev_ProcessUtility  = NULL;

/* Forward declarations for shared memory hooks. */
static void peql_shmem_request(void);
static void peql_shmem_startup(void);

/* Forward declarations for hook functions. */
static void peql_post_parse_analyze(ParseState *pstate, Query *query,
									JumbleState *jstate);
static PlannedStmt *peql_planner(Query *parse, const char *query_string,
								 int cursorOptions,
								 ParamListInfo boundParams);
static void peql_ExecutorStart(QueryDesc *queryDesc, int eflags);
static void peql_ExecutorRun(QueryDesc *queryDesc,
							 ScanDirection direction,
							 uint64 count
#if PG_VERSION_NUM < 180000
							 , bool execute_once
#endif
							 );
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
static bool peql_adaptive_check(void);
static void peql_adaptive_record(int bytes);

/* Disk space protection. */
static bool peql_disk_space_ok(void);
static void peql_try_purge_old_logs(const char *dirpath, const char *fname);

/* Plan-tree analysis. */
static bool peql_plan_walker(PlanState *planstate, void *context);
static void peql_collect_plan_metrics(QueryDesc *queryDesc,
									  PeqlPlanMetrics *metrics);

/* Wait event sampling. */
static void peql_wait_event_sample_handler(void);
static void peql_start_wait_sampling(void);
static void peql_stop_wait_sampling(void);
static void peql_format_wait_events(StringInfo buf);

/* Transaction-aware logging. */
static void peql_xact_callback(XactEvent event, void *arg);
static void peql_txn_accum_reset(void);
static void peql_write_txn_log_entry(void);

/* Parameter formatting helper. */
static void peql_append_params(StringInfo buf, ParamListInfo params);

/* Common header formatting (Time, User@Host, timestamp computation). */
static pg_time_t peql_format_header(StringInfo buf, double duration_ms);

/* SQL-callable functions. */
PG_FUNCTION_INFO_V1(pg_enhanced_query_logging_reset);
PG_FUNCTION_INFO_V1(pg_enhanced_query_logging_stats);

/*
 * GUC check hooks: reject log_filename values with path separators or
 * ".." to prevent directory traversal, and log_directory values with "..".
 */
static bool
peql_check_log_filename(char **newval, void **extra, GucSource source)
{
	if (*newval && (strchr(*newval, '/') || strchr(*newval, '\\') ||
					strstr(*newval, "..")))
	{
		GUC_check_errdetail("peql.log_filename must not contain path separators "
							"or \"..\" components.");
		return false;
	}
	return true;
}

static bool
peql_check_log_directory(char **newval, void **extra, GucSource source)
{
	if (*newval && strstr(*newval, ".."))
	{
		GUC_check_errdetail("peql.log_directory must not contain \"..\" components.");
		return false;
	}
	return true;
}

/*
 * GUC assign hooks: reset session sampling decision when rate-limit
 * parameters change, so the new value takes effect immediately.
 */
static void
peql_rate_limit_assign(int newval, void *extra)
{
	peql_session_decided = false;
}

static void
peql_rate_limit_type_assign(int newval, void *extra)
{
	peql_session_decided = false;
}

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
							   peql_check_log_directory, NULL, NULL);

	/* ---- GUC: peql.log_filename ---- */
	DefineCustomStringVariable("peql.log_filename",
							   "Name of the slow query log file.",
							   NULL,
							   &peql_log_filename,
							   "peql-slow.log",
							   PGC_SIGHUP,
							   0,
							   peql_check_log_filename, NULL, NULL);

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
							NULL, peql_rate_limit_assign, NULL);

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
							 NULL, peql_rate_limit_type_assign, NULL);

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

	/* ---- GUC: peql.rate_limit_auto_max_queries ---- */
	DefineCustomIntVariable("peql.rate_limit_auto_max_queries",
							"Max queries/second to log cluster-wide (0=off).",
							"Adaptive rate limit using shared memory counters. "
							"When the cluster-wide query count within a 1-second "
							"window exceeds this value, further entries are suppressed.",
							&peql_rate_limit_auto_max_queries,
							0,
							0, INT_MAX,
							PGC_SUSET,
							0,
							NULL, NULL, NULL);

	/* ---- GUC: peql.rate_limit_auto_max_bytes ---- */
	DefineCustomIntVariable("peql.rate_limit_auto_max_bytes",
							"Max bytes/second to log cluster-wide (0=off).",
							"Adaptive rate limit using shared memory counters. "
							"When the cluster-wide bytes written within a 1-second "
							"window exceeds this value, further entries are suppressed.",
							&peql_rate_limit_auto_max_bytes,
							0,
							0, INT_MAX,
							PGC_SUSET,
							0,
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

	/* ---- GUC: peql.track_wait_events ---- */
	DefineCustomBoolVariable("peql.track_wait_events",
							 "Sample and log wait events during execution.",
							 "Experimental. Uses a 10ms timer to sample the "
							 "backend wait event state and reports a histogram "
							 "in the log entry at full verbosity.",
							 &peql_track_wait_events,
							 false,
							 PGC_SUSET,
							 0,
							 NULL, NULL, NULL);

	/* ---- GUC: peql.log_transaction ---- */
	DefineCustomBoolVariable("peql.log_transaction",
							 "Log at transaction boundary instead of per-statement.",
							 "When enabled, metrics are accumulated across all "
							 "statements in an explicit transaction block and a "
							 "single aggregated entry is written at COMMIT/ROLLBACK.",
							 &peql_log_transaction,
							 false,
							 PGC_SUSET,
							 0,
							 NULL, NULL, NULL);

	/* ---- GUC: peql.disk_threshold_pct ---- */
	DefineCustomIntVariable("peql.disk_threshold_pct",
							"Pause logging when free disk space drops below this %.",
							"Checks the mountpoint of the log directory. "
							"0 disables the check entirely.",
							&peql_disk_threshold_pct,
							5,
							0, 100,
							PGC_SUSET,
							0,
							NULL, NULL, NULL);

	/* ---- GUC: peql.disk_check_interval_ms ---- */
	DefineCustomIntVariable("peql.disk_check_interval_ms",
							"Minimum interval (ms) between disk space checks.",
							"Limits how often statvfs() is called. "
							"Lower values detect low-disk faster but add syscall overhead.",
							&peql_disk_check_interval_ms,
							5000,
							100, INT_MAX,
							PGC_SUSET,
							GUC_UNIT_MS,
							NULL, NULL, NULL);

	/* ---- GUC: peql.disk_auto_purge ---- */
	DefineCustomBoolVariable("peql.disk_auto_purge",
							 "Delete old rotated log files when disk is low.",
							 "When enabled and disk space is below the threshold, "
							 "the extension removes .old log files it created via "
							 "pg_enhanced_query_logging_reset().",
							 &peql_disk_auto_purge,
							 false,
							 PGC_SUSET,
							 0,
							 NULL, NULL, NULL);

	/* Reserve the "peql" GUC prefix so no other extension can claim it. */
	MarkGUCPrefixReserved("peql");

	/* ---- Install shared memory hooks (for adaptive rate limiting) ---- */
	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook      = peql_shmem_request;

	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook      = peql_shmem_startup;

	/* ---- Install post_parse_analyze hook (for Query_id capture) ---- */
	prev_post_parse_analyze  = post_parse_analyze_hook;
	post_parse_analyze_hook  = peql_post_parse_analyze;

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

	/* ---- Register transaction callback (for transaction-aware logging) ---- */
	RegisterXactCallback(peql_xact_callback, NULL);
}

void
_PG_fini(void)
{
	shmem_request_hook      = prev_shmem_request_hook;
	shmem_startup_hook      = prev_shmem_startup_hook;
	post_parse_analyze_hook = prev_post_parse_analyze;
	planner_hook        = prev_planner;
	ExecutorStart_hook  = prev_ExecutorStart;
	ExecutorRun_hook    = prev_ExecutorRun;
	ExecutorFinish_hook = prev_ExecutorFinish;
	ExecutorEnd_hook    = prev_ExecutorEnd;
	ProcessUtility_hook = prev_ProcessUtility;
}

/*
 * ──────────────────────────────────────────────────────────────────────────
 * Shared memory hooks
 *
 * Request and initialise a small shared memory segment for the adaptive
 * rate limiter's cluster-wide counters.
 * ──────────────────────────────────────────────────────────────────────────
 */
static void
peql_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();

	RequestAddinShmemSpace(MAXALIGN(sizeof(PeqlSharedState)));
}

static void
peql_shmem_startup(void)
{
	bool	found;

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

	peql_shared = ShmemInitStruct("pg_enhanced_query_logging",
								  sizeof(PeqlSharedState),
								  &found);

	if (!found)
	{
		pg_atomic_init_u64(&peql_shared->queries_this_window, 0);
		pg_atomic_init_u64(&peql_shared->bytes_this_window, 0);
		pg_atomic_init_u64(&peql_shared->window_start_usec,
						   (uint64) GetCurrentTimestamp());
		pg_atomic_init_u64(&peql_shared->total_queries_logged, 0);
		pg_atomic_init_u64(&peql_shared->total_queries_skipped, 0);
		pg_atomic_init_u64(&peql_shared->total_bytes_written, 0);
		pg_atomic_init_u32(&peql_shared->disk_paused, 0);
		pg_atomic_init_u32(&peql_shared->purge_in_progress, 0);
		pg_atomic_init_u64(&peql_shared->total_disk_skipped, 0);
		pg_atomic_init_u64(&peql_shared->last_disk_check_usec, 0);
	}

	LWLockRelease(AddinShmemInitLock);
}

/*
 * ──────────────────────────────────────────────────────────────────────────
 * Adaptive rate limiter helpers
 *
 * peql_adaptive_check() returns false when the cluster-wide query or byte
 * count within the current 1-second window has reached the configured
 * maximum.  A racing window reset is handled with compare-exchange.
 *
 * peql_adaptive_record() increments the shared counters after a log
 * entry has been written.
 * ──────────────────────────────────────────────────────────────────────────
 */
static void
peql_adaptive_maybe_reset_window(void)
{
	uint64	now_usec = (uint64) GetCurrentTimestamp();
	uint64	window_start;

	window_start = pg_atomic_read_u64(&peql_shared->window_start_usec);

	if (now_usec - window_start >= 1000000)
	{
		if (pg_atomic_compare_exchange_u64(&peql_shared->window_start_usec,
										   &window_start, now_usec))
		{
			pg_atomic_write_u64(&peql_shared->queries_this_window, 0);
			pg_atomic_write_u64(&peql_shared->bytes_this_window, 0);
		}
	}
}

static bool
peql_adaptive_check(void)
{
	if (peql_shared == NULL)
		return true;

	if (peql_rate_limit_auto_max_queries <= 0 &&
		peql_rate_limit_auto_max_bytes <= 0)
		return true;

	peql_adaptive_maybe_reset_window();

	if (peql_rate_limit_auto_max_queries > 0 &&
		pg_atomic_read_u64(&peql_shared->queries_this_window) >=
		(uint64) peql_rate_limit_auto_max_queries)
		return false;

	if (peql_rate_limit_auto_max_bytes > 0 &&
		pg_atomic_read_u64(&peql_shared->bytes_this_window) >=
		(uint64) peql_rate_limit_auto_max_bytes)
		return false;

	return true;
}

static void
peql_adaptive_record(int bytes)
{
	if (peql_shared == NULL)
		return;

	if (peql_rate_limit_auto_max_queries > 0)
		pg_atomic_fetch_add_u64(&peql_shared->queries_this_window, 1);

	if (peql_rate_limit_auto_max_bytes > 0)
		pg_atomic_fetch_add_u64(&peql_shared->bytes_this_window, (uint64) bytes);
}

/*
 * ──────────────────────────────────────────────────────────────────────────
 * Disk space protection
 *
 * Checks free space on the log mountpoint using statvfs() and pauses
 * logging when it drops below peql.disk_threshold_pct.  Uses a
 * compare-and-exchange on last_disk_check_usec so that exactly one
 * backend performs the syscall per check interval, regardless of the
 * number of concurrent connections.
 *
 * On Windows, statvfs() is unavailable; the check is a no-op (always
 * returns true) until a GetDiskFreeSpaceEx() path is added.
 * ──────────────────────────────────────────────────────────────────────────
 */

/*
 * Attempt to delete old rotated log files (.old suffix) to reclaim space.
 * Only called when peql.disk_auto_purge is on and disk is low.  Protected
 * by the purge_in_progress atomic flag so only one backend purges at a time.
 */
static void
peql_try_purge_old_logs(const char *dirpath, const char *fname)
{
	DIR			   *dir;
	struct dirent  *de;
	char			pattern[MAXPGPATH];
	char			filepath[MAXPGPATH];
	int				prefix_len;

	if (!pg_atomic_test_set_flag(
			(pg_atomic_flag *) &peql_shared->purge_in_progress))
		return;

	snprintf(pattern, sizeof(pattern), "%s.old", fname);
	prefix_len = strlen(pattern);

	dir = opendir(dirpath);
	if (dir == NULL)
	{
		pg_atomic_clear_flag(
			(pg_atomic_flag *) &peql_shared->purge_in_progress);
		return;
	}

	while ((de = readdir(dir)) != NULL)
	{
		if (strncmp(de->d_name, pattern, prefix_len) != 0)
			continue;

		snprintf(filepath, sizeof(filepath), "%s/%s", dirpath, de->d_name);

		if (unlink(filepath) == 0)
			ereport(LOG,
					(errmsg("peql: disk space low, purged old log file \"%s\"",
							filepath)));
		else
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("peql: could not remove old log file \"%s\": %m",
							filepath)));
	}

	closedir(dir);

	pg_atomic_clear_flag(
		(pg_atomic_flag *) &peql_shared->purge_in_progress);
}

static bool
peql_disk_space_ok(void)
{
#ifndef WIN32
	uint64		old_usec;
	uint64		now_usec;
	uint64		interval_usec;
	char		logpath[MAXPGPATH];
	char		dirpath[MAXPGPATH];
	char	   *slash;
	struct statvfs	vfs;
	unsigned long	free_pct;
	bool		was_paused;
	bool		now_ok;

	if (peql_disk_threshold_pct <= 0 || peql_shared == NULL)
		return true;

	/* Fast path: if the check interval hasn't elapsed, use cached flag. */
	old_usec = pg_atomic_read_u64(&peql_shared->last_disk_check_usec);
	now_usec = (uint64) GetCurrentTimestamp();
	interval_usec = (uint64) peql_disk_check_interval_ms * 1000;

	if (now_usec - old_usec < interval_usec)
		return pg_atomic_read_u32(&peql_shared->disk_paused) == 0;

	/* Try to win the CAS -- only one backend checks per interval. */
	if (!pg_atomic_compare_exchange_u64(&peql_shared->last_disk_check_usec,
										&old_usec, now_usec))
		return pg_atomic_read_u32(&peql_shared->disk_paused) == 0;

	/* We won the CAS.  Resolve the log directory path. */
	peql_resolve_log_path(logpath, sizeof(logpath));
	strlcpy(dirpath, logpath, sizeof(dirpath));
	slash = strrchr(dirpath, '/');
	if (slash)
		*slash = '\0';
	else
		strlcpy(dirpath, ".", sizeof(dirpath));

	if (statvfs(dirpath, &vfs) != 0)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("peql: could not statvfs \"%s\": %m", dirpath)));
		return true;
	}

	if (vfs.f_blocks == 0)
		return true;

	free_pct = (unsigned long) ((double) vfs.f_bavail / (double) vfs.f_blocks * 100.0);
	was_paused = pg_atomic_read_u32(&peql_shared->disk_paused) != 0;
	now_ok = (free_pct >= (unsigned long) peql_disk_threshold_pct);

	if (!now_ok)
	{
		if (!was_paused)
			ereport(LOG,
					(errmsg("peql: disk space low on \"%s\" (%lu%% free, threshold %d%%), "
							"pausing query logging",
							dirpath, free_pct, peql_disk_threshold_pct)));

		pg_atomic_write_u32(&peql_shared->disk_paused, 1);

		if (peql_disk_auto_purge)
		{
			const char *fname = (peql_log_filename && peql_log_filename[0] != '\0')
				? peql_log_filename : "peql-slow.log";

			peql_try_purge_old_logs(dirpath, fname);
		}

		return false;
	}

	if (was_paused)
	{
		pg_atomic_write_u32(&peql_shared->disk_paused, 0);
		ereport(LOG,
				(errmsg("peql: disk space recovered on \"%s\" (%lu%% free), "
						"resuming query logging",
						dirpath, free_pct)));
	}

	return true;
#else
	/* Windows: no statvfs(); always allow logging for now. */
	return true;
#endif
}

/*
 * ──────────────────────────────────────────────────────────────────────────
 * Wait event sampling
 *
 * When peql.track_wait_events is on, a 10ms periodic timeout samples
 * MyProc->wait_event_info and accumulates a histogram.  The histogram
 * is formatted into a "# Wait_events:" line at full verbosity.
 * ──────────────────────────────────────────────────────────────────────────
 */
static void
peql_wait_event_sample_handler(void)
{
	uint32 info;

	if (!peql_wait_sampling_active || MyProc == NULL)
		return;

	info = MyProc->wait_event_info;

	if (info != 0)
	{
		int		i;
		bool	found = false;

		for (i = 0; i < peql_wait_sample_count; i++)
		{
			if (peql_wait_samples[i].wait_event_info == info)
			{
				peql_wait_samples[i].count++;
				found = true;
				break;
			}
		}

		if (!found && peql_wait_sample_count < PEQL_MAX_WAIT_EVENTS)
		{
			peql_wait_samples[peql_wait_sample_count].wait_event_info = info;
			peql_wait_samples[peql_wait_sample_count].count = 1;
			peql_wait_sample_count++;
		}
	}

	enable_timeout_after(peql_wait_timeout_id, 10);
}

static void
peql_start_wait_sampling(void)
{
	if (!peql_track_wait_events || !peql_active())
		return;

	peql_wait_sample_count = 0;
	memset(peql_wait_samples, 0, sizeof(peql_wait_samples));
	peql_wait_sampling_active = true;

	if (peql_wait_timeout_id == MAX_TIMEOUTS)
		peql_wait_timeout_id = RegisterTimeout(USER_TIMEOUT,
											   peql_wait_event_sample_handler);

	enable_timeout_after(peql_wait_timeout_id, 10);
}

static void
peql_stop_wait_sampling(void)
{
	if (!peql_wait_sampling_active)
		return;

	peql_wait_sampling_active = false;
	disable_timeout(peql_wait_timeout_id, false);
}

static void
peql_format_wait_events(StringInfo buf)
{
	int		i;

	if (peql_wait_sample_count == 0)
		return;

	appendStringInfoString(buf, "# Wait_events:");

	for (i = 0; i < peql_wait_sample_count; i++)
	{
		const char *type = pgstat_get_wait_event_type(peql_wait_samples[i].wait_event_info);
		const char *event = pgstat_get_wait_event(peql_wait_samples[i].wait_event_info);

		appendStringInfo(buf, " %s:%s=%d",
						 type ? type : "???",
						 event ? event : "???",
						 peql_wait_samples[i].count);
	}

	appendStringInfoChar(buf, '\n');
}

/*
 * ──────────────────────────────────────────────────────────────────────────
 * Transaction-aware logging
 * ──────────────────────────────────────────────────────────────────────────
 */
static void
peql_txn_accum_reset(void)
{
	peql_txn_accum.total_duration_ms = 0.0;
	peql_txn_accum.total_rows_sent = 0;
	peql_txn_accum.total_rows_examined = 0.0;
	peql_txn_accum.total_rows_affected = 0;
	peql_txn_accum.statement_count = 0;
	peql_txn_accum.active = false;

	if (peql_txn_accum.query_texts.data)
		resetStringInfo(&peql_txn_accum.query_texts);
	else
		initStringInfo(&peql_txn_accum.query_texts);
}

static void
peql_write_txn_log_entry(void)
{
	MemoryContext oldcxt;
	MemoryContext logcxt;

	logcxt = AllocSetContextCreate(CurrentMemoryContext,
								   "peql txn log entry",
								   ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(logcxt);

	PG_TRY();
	{
		StringInfoData	buf;
		pg_time_t		stamp_time;
		double			duration_sec;

		initStringInfo(&buf);

		duration_sec = peql_txn_accum.total_duration_ms / 1000.0;

		stamp_time = peql_format_header(&buf, peql_txn_accum.total_duration_ms);

		if (peql_log_verbosity >= PEQL_LOG_VERBOSITY_STANDARD)
		{
			const char *db = NULL;

			if (MyProcPort)
				db = MyProcPort->database_name;
			if (db == NULL) db = "";

			appendStringInfo(&buf, "# Thread_id: %d  Schema: %s  Statements: %d\n",
							 MyProcPid, db,
							 peql_txn_accum.statement_count);
		}

		appendStringInfo(&buf,
						 "# Query_time: %f  Lock_time: 0.000000"
						 "  Rows_sent: " UINT64_FORMAT
						 "  Rows_examined: %.0f\n",
						 duration_sec,
						 peql_txn_accum.total_rows_sent,
						 peql_txn_accum.total_rows_examined);

		appendStringInfo(&buf, "SET timestamp=" INT64_FORMAT ";\n",
						 (int64) stamp_time -
						 lround(peql_txn_accum.total_duration_ms / 1000.0));

		appendStringInfoString(&buf, peql_txn_accum.query_texts.data);
		if (buf.len > 0 && buf.data[buf.len - 1] != '\n')
			appendStringInfoChar(&buf, '\n');

		peql_flush_to_file(buf.data, buf.len);
		pg_atomic_fetch_add_u64(&peql_shared->total_queries_logged, 1);
		pg_atomic_fetch_add_u64(&peql_shared->total_bytes_written, (uint64) buf.len);
		peql_adaptive_record(buf.len);
	}
	PG_CATCH();
	{
		FlushErrorState();
		ereport(LOG,
				(errmsg("peql: error while writing transaction log entry, skipping")));
	}
	PG_END_TRY();

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(logcxt);
}

static void
peql_xact_callback(XactEvent event, void *arg)
{
	if (!peql_txn_accum.active)
		return;

	if (event == XACT_EVENT_COMMIT || event == XACT_EVENT_ABORT)
	{
		if (peql_txn_accum.total_duration_ms >= peql_log_min_duration &&
			peql_should_log(peql_txn_accum.total_duration_ms))
			peql_write_txn_log_entry();

		peql_txn_accum_reset();
	}
}

/*
 * ──────────────────────────────────────────────────────────────────────────
 * Post-parse-analysis hook
 *
 * Captures Query->queryId computed by the core parser (when
 * compute_query_id = on/auto) so we can emit it in the log entry for
 * cross-referencing with pg_stat_statements.
 * ──────────────────────────────────────────────────────────────────────────
 */
static void
peql_post_parse_analyze(ParseState *pstate, Query *query,
						JumbleState *jstate)
{
	if (prev_post_parse_analyze)
		prev_post_parse_analyze(pstate, query, jstate);

	if (peql_enabled && query->queryId != INT64CONST(0))
		peql_current_query_id = query->queryId;
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
	if (planner_nesting_level == 0)
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

		peql_start_wait_sampling();
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
peql_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction, uint64 count
#if PG_VERSION_NUM < 180000
				, bool execute_once
#endif
				)
{
	nesting_level++;
	PG_TRY();
	{
		if (prev_ExecutorRun)
#if PG_VERSION_NUM >= 180000
			prev_ExecutorRun(queryDesc, direction, count);
#else
			prev_ExecutorRun(queryDesc, direction, count, execute_once);
#endif
		else
#if PG_VERSION_NUM >= 180000
			standard_ExecutorRun(queryDesc, direction, count);
#else
			standard_ExecutorRun(queryDesc, direction, count, execute_once);
#endif
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
	/* Disk space check: skip logging entirely when the mountpoint is low. */
	if (!peql_disk_space_ok())
	{
		pg_atomic_fetch_add_u64(&peql_shared->total_disk_skipped, 1);
		return false;
	}

	/* Always-log override: very slow queries bypass the rate limiter. (-1 = disabled) */
	if (peql_rate_limit_always_log_duration >= 0 &&
		duration_ms >= peql_rate_limit_always_log_duration)
		return peql_adaptive_check();

	/* No rate limiting when rate_limit == 1 (log everything). */
	if (peql_rate_limit <= 1)
		return peql_adaptive_check();

	if (peql_rate_limit_type == PEQL_RATE_LIMIT_SESSION)
	{
		if (!peql_session_decided)
		{
			uint32 r = (uint32) (pg_prng_uint64(&pg_global_prng_state)
								 % (uint64) peql_rate_limit);
			peql_session_is_sampled = (r == 0);
			peql_session_decided = true;
		}
		if (!peql_session_is_sampled)
			return false;
		return peql_adaptive_check();
	}

	/* PEQL_RATE_LIMIT_QUERY: per-query draw. */
	{
		uint32 r = (uint32) (pg_prng_uint64(&pg_global_prng_state)
							 % (uint64) peql_rate_limit);
		if (r != 0)
			return false;
		return peql_adaptive_check();
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
	peql_stop_wait_sampling();

	if (queryDesc->totaltime && peql_active())
	{
		double msec;

		/* Finalize the instrumentation counters. */
		InstrEndLoop(queryDesc->totaltime);

		msec = queryDesc->totaltime->total * 1000.0;

		if (peql_log_transaction && IsTransactionBlock())
		{
			/*
			 * Transaction-aware mode: accumulate instead of writing.
			 * We accumulate all queries regardless of the per-query
			 * duration threshold; the threshold is checked against the
			 * total transaction duration at commit time.
			 */
			if (!peql_txn_accum.active)
			{
				peql_txn_accum_reset();
				peql_txn_accum.active = true;
			}

			peql_txn_accum.total_duration_ms += msec;
			peql_txn_accum.total_rows_sent += queryDesc->estate->es_processed;
			peql_txn_accum.statement_count++;

			if (queryDesc->sourceText)
			{
				if (peql_txn_accum.query_texts.len > 0)
					appendStringInfoString(&peql_txn_accum.query_texts, "\n");
				appendStringInfoString(&peql_txn_accum.query_texts,
									   queryDesc->sourceText);
				if (peql_txn_accum.query_texts.len > 0 &&
					peql_txn_accum.query_texts.data[peql_txn_accum.query_texts.len - 1] != ';')
					appendStringInfoChar(&peql_txn_accum.query_texts, ';');
			}
		}
		else if (msec >= peql_log_min_duration)
		{
			if (peql_should_log(msec))
				peql_write_log_entry(queryDesc, msec);
			else
				pg_atomic_fetch_add_u64(&peql_shared->total_queries_skipped, 1);
		}
	}

	/* Always reset so per-query state doesn't leak to the next query. */
	peql_current_plan_time_ms = 0.0;
	peql_current_query_id = 0;

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
	bool		is_execute;

	/*
	 * EXECUTE dispatches to the executor internally.  Don't bump the
	 * nesting level so the inner executor call sees nesting_level == 0
	 * and peql_active() returns true -- this lets the executor-level
	 * hooks log the underlying query together with its bound parameter
	 * values (which are only available in queryDesc->params).
	 */
	is_execute = IsA(pstmt->utilityStmt, ExecuteStmt);

	do_log = peql_log_utility && peql_enabled && peql_log_min_duration >= 0 &&
		(nesting_level == 0 || peql_log_nested);

	if (do_log)
		INSTR_TIME_SET_CURRENT(start);

	if (!is_execute)
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
		if (!is_execute)
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

		if (msec >= peql_log_min_duration)
		{
			if (peql_should_log(msec))
				peql_write_utility_log_entry(queryString, msec, params);
			else
				pg_atomic_fetch_add_u64(&peql_shared->total_queries_skipped, 1);
		}
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
	{
		int len;

		if (is_absolute_path(dir))
			len = snprintf(result, resultsize, "%s/%s", dir, fname);
		else
			len = snprintf(result, resultsize, "%s/%s/%s", DataDir, dir, fname);

		if (len >= (int) resultsize)
			ereport(LOG,
					(errmsg("peql: log file path exceeds maximum length (%d bytes)",
							(int) resultsize)));
	}
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
	 * Accumulate rows only from scan (leaf) nodes that read from storage,
	 * matching MySQL's Rows_examined semantics.  We read tuplecount (the
	 * running counter) + ntuples (already-finalised loops) because
	 * InstrEndLoop has not been called on per-node instrumentation at the
	 * point we walk the tree -- only queryDesc->totaltime gets finalised.
	 */
	switch (nodeTag(planstate))
	{
		case T_SeqScanState:
			m->has_seqscan = true;
			/* FALLTHROUGH */
		case T_IndexScanState:
		case T_IndexOnlyScanState:
		case T_BitmapHeapScanState:
		case T_TidScanState:
		case T_TidRangeScanState:
		case T_ForeignScanState:
		case T_CustomScanState:
		case T_SampleScanState:
			if (planstate->instrument)
			{
				m->rows_examined += planstate->instrument->ntuples
								  + planstate->instrument->tuplecount;

				/* Collect per-table I/O when EState is available. */
				if (m->estate)
				{
					Scan   *scan = (Scan *) planstate->plan;
					Index	scanrelid = scan->scanrelid;

					if (scanrelid > 0)
					{
						RangeTblEntry *rte = exec_rt_fetch(scanrelid, m->estate);
						Oid		relid = rte->relid;
						int64	hit  = planstate->instrument->bufusage.shared_blks_hit;
						int64	read = planstate->instrument->bufusage.shared_blks_read;
						int		i;
						bool	found = false;

						for (i = 0; i < m->table_io_count; i++)
						{
							if (m->table_io[i].relid == relid)
							{
								m->table_io[i].blks_hit  += hit;
								m->table_io[i].blks_read += read;
								found = true;
								break;
							}
						}

						if (!found && m->table_io_count < PEQL_MAX_TABLE_IO)
						{
							m->table_io[m->table_io_count].relid     = relid;
							m->table_io[m->table_io_count].blks_hit  = hit;
							m->table_io[m->table_io_count].blks_read = read;
							m->table_io_count++;
						}
					}
				}
			}
			break;
		default:
			break;
	}

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

	if (IsA(planstate, MaterialState) || IsA(planstate, CteScanState))
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

	if (peql_log_verbosity >= PEQL_LOG_VERBOSITY_FULL)
		metrics->estate = queryDesc->estate;

	if (queryDesc->planstate)
		(void) peql_plan_walker(queryDesc->planstate, metrics);
}

/*
 * ──────────────────────────────────────────────────────────────────────────
 * Common header formatter
 *
 * Emits the # Time: and # User@Host: lines shared by both query and
 * utility entry formatters. Returns stamp_time for SET timestamp.
 * ──────────────────────────────────────────────────────────────────────────
 */
static pg_time_t
peql_format_header(StringInfo buf, double duration_ms)
{
	TimestampTz	now;
	pg_time_t	stamp_time;
	char		timebuf[128];
	int			usec;
	const char *user  = NULL;
	const char *host  = NULL;

	if (MyProcPort)
	{
		user = MyProcPort->user_name;
		host = MyProcPort->remote_host;
	}
	if (user == NULL) user = "";
	if (host == NULL) host = "";

	now = GetCurrentTimestamp();
	stamp_time = timestamptz_to_time_t(now);

	{
		struct pg_tm tm_result;
		fsec_t		fsec;
		int			tz;

		if (timestamp2tm(now, &tz, &tm_result, &fsec, NULL, NULL) != 0)
		{
			snprintf(timebuf, sizeof(timebuf), "0000-00-00T00:00:00.000000");
			usec = 0;
		}
		else
		{
			usec = (int) fsec;
			snprintf(timebuf, sizeof(timebuf),
					 "%04d-%02d-%02dT%02d:%02d:%02d.%06d",
					 tm_result.tm_year,
					 tm_result.tm_mon,
					 tm_result.tm_mday,
					 tm_result.tm_hour,
					 tm_result.tm_min,
					 tm_result.tm_sec,
					 usec);
		}
	}

	appendStringInfo(buf, "# Time: %s\n", timebuf);
	appendStringInfo(buf, "# User@Host: %s[%s] @ %s []\n", user, user, host);

	return stamp_time;
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
 * Standard verbosity adds: Thread_id, Schema (db.schema), Rows_affected
 *
 * Full verbosity adds: buffer/WAL/JIT metrics, plan quality booleans,
 *   planning time, memory usage
 * ──────────────────────────────────────────────────────────────────────────
 */
static void
peql_format_entry(StringInfo buf, QueryDesc *queryDesc, double duration_ms)
{
	pg_time_t	stamp_time;
	const char *db    = NULL;
	uint64		rows_processed;
	double		duration_sec;
	const char *query_text;

	Instrumentation *instr;
	PeqlPlanMetrics	plan_metrics;
	bool			have_plan_metrics = false;

	if (MyProcPort)
		db = MyProcPort->database_name;
	if (db == NULL) db = "";

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

	/* ---- Common header: # Time: and # User@Host: ---- */
	stamp_time = peql_format_header(buf, duration_ms);

	/*
	 * ---- Standard verbosity: Thread_id, Schema (db.schema) ----
	 *
	 * Schema now uses "dbname.schemaname" format, with the current
	 * schema obtained from the search path.
	 */
	if (peql_log_verbosity >= PEQL_LOG_VERBOSITY_STANDARD)
	{
		const char *schema_name = NULL;

		PG_TRY();
		{
			List   *search_path = fetch_search_path(false);
			if (search_path != NIL)
			{
				Oid		first_ns = linitial_oid(search_path);

				schema_name = get_namespace_name(first_ns);
				list_free(search_path);
			}
		}
		PG_CATCH();
		{
			FlushErrorState();
			schema_name = NULL;
		}
		PG_END_TRY();

		if (schema_name)
			appendStringInfo(buf, "# Thread_id: %d  Schema: %s.%s  Last_errno: 0  Killed: 0\n",
							 MyProcPid, db, schema_name);
		else
			appendStringInfo(buf, "# Thread_id: %d  Schema: %s  Last_errno: 0  Killed: 0\n",
							 MyProcPid, db);

		if (queryDesc->planstate && queryDesc->planstate->plan)
		{
			int		plan_width = queryDesc->planstate->plan->plan_width;
			int64	bytes_sent = (int64) rows_processed * plan_width;

			appendStringInfo(buf, "# Bytes_sent: " INT64_FORMAT "\n", bytes_sent);
		}
		else
		{
			appendStringInfoString(buf, "# Bytes_sent: 0\n");
		}

		if (peql_current_query_id != INT64CONST(0))
			appendStringInfo(buf, "# Query_id: " INT64_FORMAT "\n",
							 peql_current_query_id);
	}

	/*
	 * ---- # Query_time / Lock_time / Rows_sent / Rows_examined ----
	 *
	 * Core line required by pt-query-digest.  When we have plan metrics
	 * from the tree walk, Rows_examined comes from actual scan-node
	 * ntuples counts rather than being a copy of Rows_sent.
	 */
	{
		/*
		 * Determine whether rows are sent to the client. SELECT INTO and
		 * CTAS have CMD_SELECT but route to DestIntoRel / DestTransientRel;
		 * DML with RETURNING does send rows despite being CMD_INSERT etc.
		 */
		bool is_returning = (queryDesc->operation != CMD_SELECT &&
							 queryDesc->plannedstmt->hasReturning);
		bool sends_rows = false;

		if (queryDesc->operation == CMD_SELECT)
		{
			CommandDest dest = queryDesc->dest ? queryDesc->dest->mydest
											   : DestNone;
			sends_rows = (dest != DestIntoRel && dest != DestTransientRel);
		}
		else if (is_returning)
		{
			sends_rows = true;
		}

		if (sends_rows)
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

		/* Rows_affected for non-SELECT or SELECT INTO/CTAS. */
		if (peql_log_verbosity >= PEQL_LOG_VERBOSITY_STANDARD && !sends_rows)
		{
			appendStringInfo(buf, "# Rows_affected: " UINT64_FORMAT "\n",
							 rows_processed);
		}
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

		/* ---- Per-table I/O attribution ---- */
		if (have_plan_metrics && plan_metrics.table_io_count > 0)
		{
			int		n = plan_metrics.table_io_count;
			int		i, j;

			/* Sort by total I/O (hit + read) descending via simple selection sort. */
			for (i = 0; i < n - 1; i++)
			{
				int max_idx = i;
				int64 max_io = plan_metrics.table_io[i].blks_hit +
							   plan_metrics.table_io[i].blks_read;

				for (j = i + 1; j < n; j++)
				{
					int64 io = plan_metrics.table_io[j].blks_hit +
							   plan_metrics.table_io[j].blks_read;
					if (io > max_io)
					{
						max_io = io;
						max_idx = j;
					}
				}
				if (max_idx != i)
				{
					PeqlTableIO tmp = plan_metrics.table_io[i];
					plan_metrics.table_io[i] = plan_metrics.table_io[max_idx];
					plan_metrics.table_io[max_idx] = tmp;
				}
			}

			appendStringInfoString(buf, "# Table_io:");

			for (i = 0; i < n && i < 5; i++)
			{
				const char *relname = NULL;
				const char *nspname = NULL;

				PG_TRY();
				{
					relname = get_rel_name(plan_metrics.table_io[i].relid);
					if (relname)
						nspname = get_namespace_name(
									get_rel_namespace(plan_metrics.table_io[i].relid));
				}
				PG_CATCH();
				{
					FlushErrorState();
					relname = NULL;
					nspname = NULL;
				}
				PG_END_TRY();

				if (relname)
				{
					appendStringInfo(buf, " %s.%s (hit=" INT64_FORMAT " read=" INT64_FORMAT ")",
									 nspname ? nspname : "???",
									 relname,
									 plan_metrics.table_io[i].blks_hit,
									 plan_metrics.table_io[i].blks_read);
					if (i < n - 1 && i < 4)
						appendStringInfoChar(buf, ',');
				}
			}
			appendStringInfoChar(buf, '\n');
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
								 "# JIT_functions: " UINT64_FORMAT
								 "  JIT_generation_time: %f"
								 "  JIT_inlining_time: %f"
								 "  JIT_optimization_time: %f"
								 "  JIT_emission_time: %f\n",
								 (uint64) ji.created_functions,
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

			appendStringInfo(buf, "# Mem_allocated: " UINT64_FORMAT "\n", (uint64) mem);
		}

		/* ---- Wait event histogram ---- */
		if (peql_track_wait_events)
			peql_format_wait_events(buf);
	}

	/* ---- Rate limit metadata (all verbosity levels when rate_limit > 1) ---- */
	if (peql_rate_limit > 1)
	{
		appendStringInfo(buf,
						 "# Log_slow_rate_type: %s  Log_slow_rate_limit: %d"
						 "  Log_slow_rate_limit_always_log_duration: %d\n",
						 (peql_rate_limit_type == PEQL_RATE_LIMIT_SESSION)
						 ? "session" : "query",
						 peql_rate_limit,
						 peql_rate_limit_always_log_duration);
	}

	if (peql_rate_limit_auto_max_queries > 0 || peql_rate_limit_auto_max_bytes > 0)
	{
		appendStringInfo(buf,
						 "# Log_slow_rate_auto_max_queries: %d"
						 "  Log_slow_rate_auto_max_bytes: %d\n",
						 peql_rate_limit_auto_max_queries,
						 peql_rate_limit_auto_max_bytes);
	}

	/* ---- SET timestamp line (query start time, not completion time) ---- */
	appendStringInfo(buf, "SET timestamp=" INT64_FORMAT ";\n",
					 (int64) stamp_time - lround(duration_ms / 1000.0));

	/* ---- Query text ---- */
	appendStringInfoString(buf, query_text);

	if (buf->len > 0 && buf->data[buf->len - 1] != ';')
		appendStringInfoChar(buf, ';');
	appendStringInfoChar(buf, '\n');

	/* ---- Bind parameter values ---- */
	if (peql_log_parameter_values && queryDesc->params)
		peql_append_params(buf, queryDesc->params);

	/* ---- EXPLAIN plan output (wrapped in PG_TRY for parallel safety) ---- */
	if (peql_log_query_plan && queryDesc->planstate)
	{
		PG_TRY();
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

			appendStringInfoString(buf, "# Plan:\n# ");
			for (const char *p = es->str->data; *p; p++)
			{
				appendStringInfoChar(buf, *p);
				if (*p == '\n')
					appendStringInfoString(buf, "# ");
			}
			appendStringInfoChar(buf, '\n');
		}
		PG_CATCH();
		{
			FlushErrorState();
			appendStringInfoString(buf, "# Plan: <unavailable>\n");
		}
		PG_END_TRY();
	}
}

/*
 * ──────────────────────────────────────────────────────────────────────────
 * File writer
 *
 * Appends pre-formatted data to the slow query log file.  Uses POSIX
 * open/write/close with O_APPEND instead of stdio fopen/fwrite/fclose
 * to guarantee a single atomic write(2) syscall per entry.  This avoids
 * interleaving when multiple backends write concurrently and the entry
 * exceeds the stdio buffer size.
 *
 * This runs from ExecutorEnd where the ResourceOwner may be in an
 * unusual state (e.g. during transaction abort or backend exit), so we
 * use raw syscalls instead of AllocateFile/FreeFile.
 * ──────────────────────────────────────────────────────────────────────────
 */
static void
peql_flush_to_file(const char *data, int len)
{
	char	logpath[MAXPGPATH];
	char	dirpath[MAXPGPATH];
	char   *slash;
	int		fd;

	peql_resolve_log_path(logpath, sizeof(logpath));

	fd = open(logpath, O_WRONLY | O_APPEND | O_CREAT, pg_file_create_mode);
	if (fd < 0)
	{
		/*
		 * If the directory doesn't exist, try to create it and retry.
		 * This mirrors PostgreSQL's own syslogger behaviour.
		 */
		strlcpy(dirpath, logpath, sizeof(dirpath));

		slash = strrchr(dirpath, '/');
		if (slash)
			*slash = '\0';

		(void) MakePGDirectory(dirpath);

		fd = open(logpath, O_WRONLY | O_APPEND | O_CREAT, pg_file_create_mode);
		if (fd < 0)
		{
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("peql: could not open log file \"%s\": %m",
							logpath)));
			return;
		}
	}

	{
		ssize_t written = write(fd, data, len);
		if (written != (ssize_t) len)
		{
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("peql: short write to \"%s\": wrote %zd of %d bytes",
							logpath, written, len)));
		}
	}
	close(fd);
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
		ParamExternData prmdata;
		ParamExternData *prm;

		if (params->paramFetch)
			prm = params->paramFetch(params, i + 1, false, &prmdata);
		else
			prm = &params->params[i];

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
			appendStringInfoChar(buf, '\'');
			for (const char *p = val; *p; p++)
			{
				switch (*p)
				{
					case '\'':
						appendStringInfoString(buf, "''");
						break;
					case '\n':
						appendStringInfoString(buf, "\\n");
						break;
					case '\r':
						appendStringInfoString(buf, "\\r");
						break;
					case '\\':
						appendStringInfoString(buf, "\\\\");
						break;
					default:
						appendStringInfoChar(buf, *p);
						break;
				}
			}
			appendStringInfoChar(buf, '\'');
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
	pg_time_t	stamp_time;
	const char *db    = NULL;
	double		duration_sec;

	if (MyProcPort)
		db = MyProcPort->database_name;
	if (db == NULL) db = "";

	duration_sec = duration_ms / 1000.0;

	/* ---- Common header: # Time: and # User@Host: ---- */
	stamp_time = peql_format_header(buf, duration_ms);

	if (peql_log_verbosity >= PEQL_LOG_VERBOSITY_STANDARD)
	{
		const char *schema_name = NULL;

		PG_TRY();
		{
			List   *search_path = fetch_search_path(false);
			if (search_path != NIL)
			{
				Oid		first_ns = linitial_oid(search_path);

				schema_name = get_namespace_name(first_ns);
				list_free(search_path);
			}
		}
		PG_CATCH();
		{
			FlushErrorState();
			schema_name = NULL;
		}
		PG_END_TRY();

		if (schema_name)
			appendStringInfo(buf, "# Thread_id: %d  Schema: %s.%s  Last_errno: 0  Killed: 0\n",
							 MyProcPid, db, schema_name);
		else
			appendStringInfo(buf, "# Thread_id: %d  Schema: %s  Last_errno: 0  Killed: 0\n",
							 MyProcPid, db);

		appendStringInfoString(buf, "# Bytes_sent: 0\n");

		if (peql_current_query_id != INT64CONST(0))
			appendStringInfo(buf, "# Query_id: " INT64_FORMAT "\n",
							 peql_current_query_id);
	}

	appendStringInfo(buf,
					 "# Query_time: %f  Lock_time: 0.000000"
					 "  Rows_sent: 0  Rows_examined: 0\n",
					 duration_sec);

	if (peql_rate_limit > 1)
	{
		appendStringInfo(buf,
						 "# Log_slow_rate_type: %s  Log_slow_rate_limit: %d"
						 "  Log_slow_rate_limit_always_log_duration: %d\n",
						 (peql_rate_limit_type == PEQL_RATE_LIMIT_SESSION)
						 ? "session" : "query",
						 peql_rate_limit,
						 peql_rate_limit_always_log_duration);
	}

	if (peql_rate_limit_auto_max_queries > 0 || peql_rate_limit_auto_max_bytes > 0)
	{
		appendStringInfo(buf,
						 "# Log_slow_rate_auto_max_queries: %d"
						 "  Log_slow_rate_auto_max_bytes: %d\n",
						 peql_rate_limit_auto_max_queries,
						 peql_rate_limit_auto_max_bytes);
	}

	/* SET timestamp reflects query start time, not completion time. */
	appendStringInfo(buf, "SET timestamp=" INT64_FORMAT ";\n",
					 (int64) stamp_time - lround(duration_ms / 1000.0));
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
	MemoryContext oldcxt;
	MemoryContext logcxt;

	logcxt = AllocSetContextCreate(CurrentMemoryContext,
								   "peql log entry",
								   ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(logcxt);

	PG_TRY();
	{
		StringInfoData buf;

		initStringInfo(&buf);
		peql_format_entry(&buf, queryDesc, duration_ms);
		peql_flush_to_file(buf.data, buf.len);
		pg_atomic_fetch_add_u64(&peql_shared->total_queries_logged, 1);
		pg_atomic_fetch_add_u64(&peql_shared->total_bytes_written, (uint64) buf.len);
		peql_adaptive_record(buf.len);
	}
	PG_CATCH();
	{
		FlushErrorState();
		ereport(LOG,
				(errmsg("peql: error while writing log entry, skipping")));
	}
	PG_END_TRY();

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(logcxt);
}

/*
 * Writer for utility (DDL) log entries.
 */
static void
peql_write_utility_log_entry(const char *queryString, double duration_ms,
							 ParamListInfo params)
{
	MemoryContext oldcxt;
	MemoryContext logcxt;

	logcxt = AllocSetContextCreate(CurrentMemoryContext,
								   "peql utility log entry",
								   ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(logcxt);

	PG_TRY();
	{
		StringInfoData buf;

		initStringInfo(&buf);
		peql_format_utility_entry(&buf, queryString, duration_ms, params);
		peql_flush_to_file(buf.data, buf.len);
		pg_atomic_fetch_add_u64(&peql_shared->total_queries_logged, 1);
		pg_atomic_fetch_add_u64(&peql_shared->total_bytes_written, (uint64) buf.len);
		peql_adaptive_record(buf.len);
	}
	PG_CATCH();
	{
		FlushErrorState();
		ereport(LOG,
				(errmsg("peql: error while writing utility log entry, skipping")));
	}
	PG_END_TRY();

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(logcxt);
}

/*
 * ──────────────────────────────────────────────────────────────────────────
 * SQL-callable functions
 * ──────────────────────────────────────────────────────────────────────────
 */

/*
 * pg_enhanced_query_logging_reset()
 *
 * Rotates the slow query log file by renaming it to .old, so concurrent
 * writers finish safely on the old inode and new writes create a fresh file.
 * Returns true on success.
 */
Datum
pg_enhanced_query_logging_reset(PG_FUNCTION_ARGS)
{
	char	logpath[MAXPGPATH];
	char	oldpath[MAXPGPATH];

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to reset the enhanced query log")));

	peql_resolve_log_path(logpath, sizeof(logpath));

	if (snprintf(oldpath, sizeof(oldpath), "%s.old", logpath) >= (int) sizeof(oldpath))
		ereport(LOG,
				(errmsg("peql: rotated log path exceeds maximum length (%d bytes)",
						(int) sizeof(oldpath))));

	/*
	 * Rename instead of truncate to avoid racing with concurrent writers.
	 * Backends with the old file still open will finish writing to the
	 * renamed inode; new opens will create a fresh file.
	 */
	if (rename(logpath, oldpath) != 0 && errno != ENOENT)
	{
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("peql: could not rename log file \"%s\" to \"%s\": %m",
						logpath, oldpath)));
		PG_RETURN_BOOL(false);
	}

	pg_atomic_write_u64(&peql_shared->total_queries_logged, 0);
	pg_atomic_write_u64(&peql_shared->total_queries_skipped, 0);
	pg_atomic_write_u64(&peql_shared->total_bytes_written, 0);
	pg_atomic_write_u64(&peql_shared->total_disk_skipped, 0);

	ereport(NOTICE,
			(errmsg("peql: log file \"%s\" has been rotated to \"%s\"",
					logpath, oldpath)));

	PG_RETURN_BOOL(true);
}

/*
 * pg_enhanced_query_logging_stats()
 *
 * Returns global logging counters: queries_logged, queries_skipped,
 * bytes_written, disk_paused, disk_skipped.  These are aggregated across
 * all backends via shared memory atomics and track activity since the
 * last reset (or server start).
 */
Datum
pg_enhanced_query_logging_stats(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Datum		values[5];
	bool		nulls[5] = {false, false, false, false, false};

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("function returning record called in context that "
						"cannot accept type record")));

	tupdesc = BlessTupleDesc(tupdesc);

	values[0] = Int64GetDatum((int64) pg_atomic_read_u64(&peql_shared->total_queries_logged));
	values[1] = Int64GetDatum((int64) pg_atomic_read_u64(&peql_shared->total_queries_skipped));
	values[2] = Int64GetDatum((int64) pg_atomic_read_u64(&peql_shared->total_bytes_written));
	values[3] = BoolGetDatum(pg_atomic_read_u32(&peql_shared->disk_paused) != 0);
	values[4] = Int64GetDatum((int64) pg_atomic_read_u64(&peql_shared->total_disk_skipped));

	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}
