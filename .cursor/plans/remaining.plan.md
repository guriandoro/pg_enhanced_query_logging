---
name: Remaining features from implementation plan
overview: |
  Features described in the original implementation.plan.md that have no corresponding code yet.  Items 1-4 were specified in the main plan body (GUC table, SQL interface, output format, metrics mapping) and should be considered core gaps.  Items 5-7 come from the "Novel Ideas" section and are stretch goals.
todos:
  - id: stats-function
    content: |
      Implement pg_enhanced_query_logging_stats() SQL function that returns internal counters: queries_logged, queries_skipped, bytes_written.
    status: completed
  - id: bytes-sent
    content: |
      Emit "# Bytes_sent: N" in the log output at standard+ verbosity, estimated from the result set.
    status: completed
  - id: query-id
    content: |
      Capture PostgreSQL's queryId via post_parse_analyze_hook and emit "# Query_id: N" in the log output for cross-referencing with pg_stat_statements.
    status: completed
  - id: adaptive-rate-limiting
    content: |
      Implement peql.rate_limit_auto_max_queries and peql.rate_limit_auto_max_bytes GUCs with shared-memory-backed adaptive rate limiting.
    status: completed
  - id: per-table-io
    content: |
      At full verbosity, log per-table I/O attribution showing which tables contributed the most buffer hits/reads.
    status: completed
  - id: wait-event-tracking
    content: |
      Capture and log wait events during query execution (IO, Lock, LWLock, etc.) from the pg_stat_activity wait_event infrastructure.
    status: completed
  - id: transaction-aware-logging
    content: |
      Option to log at transaction boundary instead of per-statement, aggregating metrics across all statements in the transaction.
    status: completed
isProject: false
---

# Remaining Features

Below is a detailed description of each unimplemented feature, including the
exact code locations to modify, the new files to create, and the step-by-step
implementation instructions an agent would need to carry out the work.

---

## 1. `pg_enhanced_query_logging_stats()` SQL Function

### What

The original plan's "SQL Interface" section specifies two SQL-callable
functions.  Only `pg_enhanced_query_logging_reset()` exists today.  The
second function, `pg_enhanced_query_logging_stats()`, should return a
composite row with internal counters so users can monitor logging activity
without reading the log file.

### Return columns


| Column          | Type   | Description                                  |
| --------------- | ------ | -------------------------------------------- |
| queries_logged  | bigint | Total log entries written since server start |
| queries_skipped | bigint | Queries that exceeded duration but were      |
|                 |        | suppressed by rate limiting                  |
| bytes_written   | bigint | Total bytes appended to the log file         |


### Implementation steps

1. **Add static counters in `pg_enhanced_query_logging.c`:**
  Near the existing rate-limiting state variables (around line 160), add
   three `static int64` counters:

```c
   static int64 peql_queries_logged  = 0;
   static int64 peql_queries_skipped = 0;
   static int64 peql_bytes_written   = 0;
   

```

   These are per-backend only (no shared memory needed for the initial
   version).

1. **Increment `peql_queries_logged` and `peql_bytes_written`:**
  In `peql_write_log_entry()` (line ~1610) and
   `peql_write_utility_log_entry()` (line ~1643), after a successful
   `peql_flush_to_file()` call, increment:

```c
   peql_queries_logged++;
   peql_bytes_written += buf.len;
   

```

1. **Increment `peql_queries_skipped`:**
  In `peql_ExecutorEnd()` (line ~757), when `msec >= peql_log_min_duration`
   but `peql_should_log(msec)` returns false, increment:

```c
   peql_queries_skipped++;
   

```

   Same logic in `peql_ProcessUtility()` (line ~839).

1. **Implement the C function:**
  Add a `PG_FUNCTION_INFO_V1(pg_enhanced_query_logging_stats)` declaration
   near the existing one for `_reset` (line ~258).  Implement as:

```c
   Datum
   pg_enhanced_query_logging_stats(PG_FUNCTION_ARGS)
   {
       TupleDesc   tupdesc;
       Datum       values[3];
       bool        nulls[3] = {false, false, false};

       if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
           ereport(ERROR, ...);

       tupdesc = BlessTupleDesc(tupdesc);

       values[0] = Int64GetDatum(peql_queries_logged);
       values[1] = Int64GetDatum(peql_queries_skipped);
       values[2] = Int64GetDatum(peql_bytes_written);

       PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
   }
   

```

1. **Add to `pg_enhanced_query_logging--1.1.sql` (or create a 1.2 upgrade):**

```sql
   CREATE FUNCTION pg_enhanced_query_logging_stats(
       OUT queries_logged  bigint,
       OUT queries_skipped bigint,
       OUT bytes_written   bigint
   )
   RETURNS record
   AS 'MODULE_PATHNAME', 'pg_enhanced_query_logging_stats'
   LANGUAGE C STRICT VOLATILE;

   REVOKE ALL ON FUNCTION pg_enhanced_query_logging_stats() FROM PUBLIC;
   

```

   Decision for the agent: either bump to version 1.2 (new `.control`
   `default_version`, new `--1.2.sql`, and `--1.1--1.2.sql` migration),
   or append to the existing `--1.1.sql` if no users are on 1.1 yet.

1. **Reset counters from `pg_enhanced_query_logging_reset()`:**
  Optionally zero the counters when the log is rotated, so they track
   activity since last reset.
2. **Add a regression test:**
  In `sql/01_basic.sql` (or a new file), call the function and verify it
   returns a valid row.  A TAP test could verify that after running N
   queries, `queries_logged` is approximately N.
3. **Update README.md:**
  Document the new function in the "SQL Functions" section.

### Files to modify

- `pg_enhanced_query_logging.c` (counters, C function, increment sites)
- `pg_enhanced_query_logging--1.1.sql` (or new version files)
- `pg_enhanced_query_logging.control` (if bumping version)
- `Makefile` DATA line (if adding new SQL files)
- `sql/` and `expected/` (regression test)
- `README.md`

---

## 2. `Bytes_sent` Metric

### What

The plan's output format specification shows a `# Bytes_sent: 4096` line
emitted at standard+ verbosity.  The metrics mapping table says it is
"Estimated from result set".  The code currently does not emit this field.

### How to estimate Bytes_sent

PostgreSQL does not expose "bytes sent to client" as an instrumentation
counter.  The best available proxy is:

- **Option A (simple):** Use `rows_processed * average_row_width` where
`average_row_width` comes from the top-level plan node's
`planstate->plan->plan_width`.  This is what the planner estimates.
- **Option B (more accurate):** Walk the `TupleTableSlot` to compute
actual sizes, but this would require per-row overhead in the executor
hot path and is not recommended.

Option A is recommended -- it matches the "estimated from result set"
description in the plan.

### Implementation steps

1. **In `peql_format_entry()` (line ~1094), inside the
  `peql_log_verbosity >= PEQL_LOG_VERBOSITY_STANDARD` block:**
   After the `Thread_id` / `Schema` line (around line 1122), compute and
   emit `Bytes_sent`:

```c
   if (queryDesc->planstate && queryDesc->planstate->plan)
   {
       int plan_width = queryDesc->planstate->plan->plan_width;
       int64 bytes_sent = (int64) rows_processed * plan_width;
       appendStringInfo(buf, "# Bytes_sent: " INT64_FORMAT "\n", bytes_sent);
   }
   

```

   The line should appear after the `# Thread_id:` line and before the
   `# Query_time:` line, matching the order shown in the plan's example
   output.

1. **For utility entries (`peql_format_utility_entry`):**
  Utility statements don't have a plan tree or result set, so emit
   `# Bytes_sent: 0` (or skip entirely).
2. **Add a TAP test:**
  Extend `t/003_extended_metrics.pl` or create a new test that verifies
   the `Bytes_sent` line appears in the log at standard or full verbosity
   and is absent at minimal verbosity.
3. **Update README.md:**
  Add `Bytes_sent` to the output format tables.

### Files to modify

- `pg_enhanced_query_logging.c` (`peql_format_entry`, optionally
`peql_format_utility_entry`)
- `t/003_extended_metrics.pl` (or new test)
- `README.md`

---

## 3. `Query_id` via `post_parse_analyze_hook`

### What

The plan's "Novel Ideas" section item 1 specifies:

> Include PostgreSQL's `queryId` (computed by `pg_stat_statements` style
> jumble) as a `Query_id` field.  This enables cross-referencing with
> `pg_stat_statements` without re-parsing queries.

The architecture diagram in the plan shows a `post_parse_analyze_hook`
being installed, but the code does not install this hook and does not
capture or emit any query ID.

### Background

Since PostgreSQL 14, `compute_query_id = on` (or `auto`) causes the
core parser to compute a `queryId` on every `Query` node.  The
`post_parse_analyze_hook` fires after parsing and can read
`query->queryId`.  In PostgreSQL 18, `compute_query_id` defaults to
`auto`, which means the ID is computed whenever any module requests it
(e.g., `pg_stat_statements`).

The extension does NOT need to compute the query ID itself -- it only
needs to read `Query->queryId` if it is non-zero.

### Implementation steps

1. **Add a per-backend variable to store the query ID:**

```c
   static uint64 peql_current_query_id = 0;
   

```

1. **Install `post_parse_analyze_hook` in `_PG_init()`:**

```c
   #include "parser/analyze.h"

   static post_parse_analyze_hook_type prev_post_parse_analyze = NULL;

   static void peql_post_parse_analyze(ParseState *pstate, Query *query,
                                       JumbleState *jstate);
   

```

   In `_PG_init()`:

```c
   prev_post_parse_analyze = post_parse_analyze_hook;
   post_parse_analyze_hook = peql_post_parse_analyze;
   

```

   In `_PG_fini()`:

```c
   post_parse_analyze_hook = prev_post_parse_analyze;
   

```

1. **Implement the hook:**

```c
   static void
   peql_post_parse_analyze(ParseState *pstate, Query *query,
                           JumbleState *jstate)
   {
       if (prev_post_parse_analyze)
           prev_post_parse_analyze(pstate, query, jstate);

       if (peql_enabled && query->queryId != UINT64CONST(0))
           peql_current_query_id = query->queryId;
   }
   

```

1. **Emit `Query_id` in `peql_format_entry()`:**
  At standard+ verbosity (or all levels), after the `# Thread_id:` line:

```c
   if (peql_current_query_id != 0)
       appendStringInfo(buf, "# Query_id: " UINT64_FORMAT "\n",
                        peql_current_query_id);
   

```

1. **Reset the query ID in `peql_ExecutorEnd()`:**
  After writing the log entry, reset:

```c
   peql_current_query_id = 0;
   

```

1. **Handle the include:**
  Add `#include "parser/analyze.h"` to the includes at the top of the
   file.  Check PostgreSQL version headers to confirm the exact header
   location -- it might be `parser/analyze.h` in PG 18.  The postgres
   source is at `~/src/postgres/` for reference.
2. **Tests:**
  Add a TAP test that:
  - Sets `compute_query_id = on` in the test server config
  - Runs a query that gets logged
  - Verifies the log output contains a `# Query_id:` line with a
  non-zero integer value
3. **Update README.md** with Query_id documentation.

### Files to modify

- `pg_enhanced_query_logging.c` (hook, variable, format, reset)
- `t/` (new or extended TAP test)
- `README.md`

---

## 4. Adaptive Rate Limiting (`rate_limit_auto`)

### What

The plan's GUC table lists two GUCs that don't exist in the code:


| GUC                                | Type | Default | Description                       |
| ---------------------------------- | ---- | ------- | --------------------------------- |
| `peql.rate_limit_auto_max_queries` | int  | 0       | Max queries/second to log (0=off) |
| `peql.rate_limit_auto_max_bytes`   | int  | 0       | Max bytes/second to log (0=off)   |


The "Novel Ideas" section describes an adaptive mode where:

- Each backend tracks its own bytes-written counter
- A **shared memory** counter aggregates across all backends
- When the aggregate exceeds the byte/query threshold within a 1-second
window, new entries are suppressed until the window resets
- Either or both throttles can be active simultaneously

### Implementation steps

1. **Request shared memory in `_PG_init()`:**
  Use `RequestAddinShmemSpace()` and `RequestAddinLWLocks()` to reserve
   space for the shared counters.  Register a `shmem_request_hook` or use
   `shmem_startup_hook` to initialize the shared memory segment.
   The shared memory struct could look like:

```c
   typedef struct PeqlSharedState
   {
       LWLock     *lock;
       pg_atomic_int64 queries_this_window;
       pg_atomic_int64 bytes_this_window;
       pg_atomic_int64 window_start_usec;
   } PeqlSharedState;
   

```

   Using `pg_atomic_int64` avoids needing the LWLock for the common
   read/increment path.

1. **Add the two GUC variables:**

```c
   static int peql_rate_limit_auto_max_queries = 0;
   static int peql_rate_limit_auto_max_bytes   = 0;
   

```

   Register with `DefineCustomIntVariable()` in `_PG_init()`, with
   range `[0, INT_MAX]`, PGC_SUSET context, 0 default (disabled).

1. **Modify `peql_should_log()` to check adaptive limits:**
  After the existing rate-limit checks, if either `max_queries` or
   `max_bytes` is > 0:
  - Read the current window start from shared memory
  - If more than 1 second has passed, atomically reset the window
  - If `queries_this_window >= max_queries` (when max_queries > 0),
  return false
  - If `bytes_this_window >= max_bytes` (when max_bytes > 0),
  return false (note: for byte tracking, the caller doesn't know
  the entry size yet -- so this check is done post-format in the
  write path, or uses the *previous* window's byte count as a
  predictor)
2. **Increment shared counters after writing:**
  In `peql_flush_to_file()`, after a successful write, atomically
   increment `queries_this_window` and add `len` to
   `bytes_this_window`.
3. **Handle the timing window:**
  Use `GetCurrentTimestamp()` to determine the current window.
   Each window is 1 second.  When the current time exceeds
   `window_start + 1 second`, atomically reset both counters and
   update `window_start`.  Use `pg_atomic_compare_exchange_u64` to
   avoid races on reset.
4. **Emit metadata in log entry:**
  When adaptive rate limiting is active, optionally emit:

```
   # Log_slow_rate_auto_max_queries: 100
   # Log_slow_rate_auto_max_bytes: 2097152
   

```

1. **Update `pg_enhanced_query_logging.conf`** with the new GUCs.
2. **Tests:**
  - Regression test: verify the GUCs accept valid values and reject
   invalid ones (negative numbers)
  - TAP test: set `max_queries = 5`, run 20 fast queries in rapid
  succession, verify that roughly 5 are logged per second
3. **Update README.md** with the new GUC documentation.

### Complexity note

This is the most complex remaining feature because it requires shared
memory, atomic operations, and careful race-condition handling.  The
extension currently does not use `shmem_startup_hook` or shared memory
at all, so this is greenfield infrastructure.

### Files to modify

- `pg_enhanced_query_logging.c` (shared memory, GUCs, rate limiter, format)
- `pg_enhanced_query_logging.conf`
- `sql/02_guc.sql` + `expected/02_guc.out` (GUC validation)
- `t/` (new TAP test)
- `README.md`

---

## 5. Per-Table I/O Attribution (Stretch Goal)

### What

At full verbosity, log which tables contributed the most buffer
hits/reads by leveraging `PlanState` instrumentation per scan node.

Example output:

```
# Table_io: public.orders (hit=100 read=42), public.order_items (hit=30 read=5)
```

### Implementation steps

1. **Extend `PeqlPlanMetrics` or create a separate struct:**
  Add an array or linked list of `(table_oid, blks_hit, blks_read)`
   entries.
2. **Modify `peql_plan_walker()`:**
  For scan nodes (SeqScan, IndexScan, etc.), extract the relation OID
   from `planstate->plan` (e.g., `((Scan *) planstate->plan)->scanrelid`
   resolved via `rt_fetch()` on the range table).  Read per-node
   `Instrumentation->bufusage` for hit/read counts and accumulate by
   table OID.
   Note: per-node `bufusage` is only populated when `INSTRUMENT_BUFFERS`
   is included in `instrument_options`, which the code already does at
   full verbosity (`INSTRUMENT_ALL`).
3. **Resolve OID to schema.table name:**
  Use `get_rel_name()` and `get_namespace_name(get_rel_namespace())`
   to convert the OID to a human-readable name.  Wrap in PG_TRY since
   the relation might have been dropped concurrently.
4. **Sort by total I/O (hit + read) descending, emit top N (e.g., 5).**
5. **Emit as `# Table_io: ...` in `peql_format_entry()`** inside the
  full-verbosity block.
6. **Tests:** TAP test that creates a table, runs a sequential scan,
  verifies the table name appears in `Table_io` line.

### Files to modify

- `pg_enhanced_query_logging.c` (walker, format, struct)
- `t/` (new TAP test)
- `README.md`

---

## 6. Wait Event Tracking (Stretch Goal)

### What

Capture wait events during query execution from PostgreSQL's
`pg_stat_activity` wait_event infrastructure, showing what the query
was waiting on (IO, Lock, LWLock, etc.).

Example output:

```
# Wait_events: IO:DataFileRead=3 Lock:relation=1 LWLock:BufferContent=2
```

### Background

PostgreSQL tracks wait events per-backend in shared memory via
`PGPROC->wait_event_info`.  However, this is a *current state* snapshot,
not an accumulated history.  There is no built-in way to get a histogram
of wait events that occurred during a single query execution.

### Possible approaches

**Approach A -- Periodic sampling (recommended):**

Use a background worker or signal-based timer to sample
`MyProc->wait_event_info` at a configurable interval (e.g., every 1ms)
during query execution.  Accumulate counts in a per-backend hash table.
This is how `pg_wait_sampling` works.

Drawbacks: adds timer overhead; sampling can miss short waits.

**Approach B -- Hook into wait event reporting:**

PostgreSQL has `pgstat_report_wait_start()` / `pgstat_report_wait_end()`.
These are macros/inline functions, not hookable.  Patching them would
require modifying PostgreSQL core, which defeats the purpose of an
extension.

**Approach C -- Read /proc or dtrace probes:**

Platform-specific and fragile.  Not recommended.

### Implementation steps (Approach A)

1. **Add a `peql.track_wait_events` GUC** (bool, default false).
2. **In `peql_ExecutorStart()*`*, if tracking is enabled, set up a
  `SIGALRM`-based timer or use `enable_timeout()` to fire every N ms.
3. **In the timer handler**, read `MyProc->wait_event_info`, decode it
  via `pgstat_get_wait_event_type()` and `pgstat_get_wait_event()`,
   and increment a per-event counter in a hash table.
4. **In `peql_ExecutorEnd()`**, stop the timer, format the accumulated
  counters into a `# Wait_events:` line, and reset the hash table.
5. **Tests:** Hard to test deterministically.  A smoke test could run
  a query that forces I/O waits (e.g., on a cold cache) and verify
   the `Wait_events` line appears.

### Complexity note

This is architecturally complex and has performance implications.
Consider making it a separate `.c` file or even a separate extension
that cooperates with PEQL.

### Files to modify

- `pg_enhanced_query_logging.c` (or new file)
- `t/` (new TAP test)
- `README.md`

---

## 7. Transaction-Aware Logging (Stretch Goal)

### What

Option to log at transaction boundary instead of per-statement,
aggregating metrics across all statements in a transaction.

Example output:

```
# Time: 2026-02-27T14:30:00.123456
# User@Host: alice[alice] @ 192.168.1.10 []
# Transaction_id: 12345  Statements: 5
# Query_time: 1.234567  Lock_time: 0.000300  Rows_sent: 142  Rows_examined: 50000
SET timestamp=1772147399;
BEGIN;
INSERT INTO orders ...;
INSERT INTO order_items ...;
UPDATE inventory ...;
SELECT * FROM orders WHERE id = 42;
COMMIT;
```

### Implementation steps

1. **Add GUCs:**

```c
   static bool peql_log_transaction = false;  /* PGC_SUSET */
   

```

   When enabled, per-statement logging is suppressed and metrics are
   accumulated.  At `COMMIT` / `ROLLBACK`, a single aggregated entry
   is written.

1. **Add per-backend accumulator state:**

```c
   typedef struct PeqlTxnAccumulator
   {
       double      total_duration_ms;
       double      total_lock_time;
       uint64      total_rows_sent;
       double      total_rows_examined;
       uint64      total_rows_affected;
       BufferUsage total_bufusage;
       WalUsage    total_walusage;
       int         statement_count;
       StringInfoData query_texts;
       bool        active;
   } PeqlTxnAccumulator;

   static PeqlTxnAccumulator peql_txn_accum;
   

```

1. **In `peql_ExecutorEnd()`:**
  When `peql_log_transaction` is true and we are inside a transaction
   block (`IsTransactionBlock()`), instead of writing a log entry,
   accumulate the metrics into `peql_txn_accum` and append the query
   text to `query_texts`.
2. **Hook into transaction commit/abort:**
  Use `RegisterXactCallback()` in `_PG_init()` to register a callback
   that fires on `XACT_EVENT_COMMIT` and `XACT_EVENT_ABORT`.  In the
   callback:
  - If `peql_txn_accum.active` is true and total_duration_ms exceeds
  the threshold, format and write the aggregated entry
  - Reset the accumulator
3. **Format the aggregated entry:**
  Similar to `peql_format_entry()` but uses accumulated totals and
   concatenated query texts.  Add a `# Transaction_id:` line and a
   `# Statements: N` line.
4. **Handle single-statement implicit transactions:**
  When a query runs outside an explicit `BEGIN` block, PostgreSQL wraps
   it in an implicit transaction.  These should still log per-statement
   as today (check `IsTransactionBlock()` to distinguish).
5. **Tests:**
  TAP test that:
  - Begins a transaction, runs several queries, commits
  - Verifies a single log entry is written containing all queries
  - Verifies that metrics are aggregated

### Files to modify

- `pg_enhanced_query_logging.c` (accumulator, xact callback, format)
- `t/` (new TAP test)
- `README.md`

---

## Priority and Dependencies


| #   | Feature                   | Complexity | Dependencies   | Recommended order |
| --- | ------------------------- | ---------- | -------------- | ----------------- |
| 1   | `stats()` function        | Low        | None           | 1st               |
| 2   | `Bytes_sent` metric       | Low        | None           | 2nd               |
| 3   | `Query_id`                | Low-Medium | None           | 3rd               |
| 4   | Adaptive rate limiting    | High       | Shared memory  | 4th               |
| 5   | Per-table I/O             | Medium     | None           | 5th               |
| 6   | Wait event tracking       | High       | Timer/sampling | 6th               |
| 7   | Transaction-aware logging | Medium     | Xact callbacks | 7th               |


Items 1-3 can be implemented independently and in parallel.
Item 4 is standalone but requires shared memory infrastructure.
Items 5-7 are independent of each other but depend on the core being stable.