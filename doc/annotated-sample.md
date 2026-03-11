# Annotated Sample Log Entry

[Back to README](../README.md)

This page shows a single PEQL slow log entry with **every optional feature enabled**, followed by a section-by-section breakdown explaining what each part means and which GUC must be set to produce it.

## Full Configuration

The following `postgresql.conf` settings produce the entry shown below:

```
shared_preload_libraries = 'pg_enhanced_query_logging'

# PostgreSQL settings required by some PEQL features
compute_query_id   = on    # needed for Query_id
track_io_timing    = on    # needed for block read/write times

# Core
peql.enabled            = on
peql.log_min_duration   = 0        # log every query
peql.log_verbosity      = 'full'   # emit all metric lines

# Extended tracking
peql.track_io_timing    = on       # block read/write times
peql.track_wal          = on       # WAL records/bytes/fpi
peql.track_planning     = on       # separate planning time
peql.track_memory       = on       # executor memory context
peql.track_wait_events  = on       # wait event histogram

# Query plan and parameters
peql.log_query_plan          = on
peql.log_query_plan_format   = 'text'
peql.log_parameter_values    = on

# Rate limiting (set > 1 to see rate limit metadata lines)
peql.rate_limit                      = 10
peql.rate_limit_type                 = 'query'
peql.rate_limit_always_log_duration  = 5000
```

## The Log Entry

```
# Time: 2026-03-11T09:15:32.847291
# User@Host: app_user[app_user] @ 10.0.1.42 []
# Thread_id: 48712  Schema: mydb.public
# Bytes_sent: 16384
# Query_id: -6432758210044805760
# Query_time: 1.285034  Lock_time: 0.000000  Rows_sent: 256  Rows_examined: 87500
# Rows_affected: 0
# Shared_blks_hit: 4096  Shared_blks_read: 312  Shared_blks_dirtied: 0  Shared_blks_written: 0
# Local_blks_hit: 0  Local_blks_read: 0  Local_blks_dirtied: 0  Local_blks_written: 0
# Temp_blks_read: 0  Temp_blks_written: 48
# Shared_blk_read_time: 0.024310  Shared_blk_write_time: 0.000000
# WAL_records: 0  WAL_bytes: 0  WAL_fpi: 0
# Plan_time: 0.003210
# Full_scan: Yes  Temp_table: No  Temp_table_on_disk: Yes  Filesort: Yes  Filesort_on_disk: No
# Table_io: public.orders (hit=3800 read=280), public.customers (hit=296 read=32)
# JIT_functions: 4  JIT_generation_time: 0.001250  JIT_inlining_time: 0.000830  JIT_optimization_time: 0.002410  JIT_emission_time: 0.003100
# Mem_allocated: 2097152
# Wait_events: IO:DataFileRead=12, Client:ClientRead=3
# Log_slow_rate_type: query  Log_slow_rate_limit: 10  Log_slow_rate_limit_always_log_duration: 5000
SET timestamp=1741680931;
SELECT o.id, o.total, c.name
  FROM orders o
  JOIN customers c ON c.id = o.customer_id
 WHERE o.status = $1
   AND o.created_at > $2
 ORDER BY o.total DESC
 LIMIT 256;
# Parameters: $1 = 'pending', $2 = '2026-01-01 00:00:00+00'
# Plan:
# Sort  (cost=1850.23..1850.87 rows=256 width=64) (actual time=1.280..1.283 rows=256 loops=1)
#   Sort Key: o.total DESC
#   Sort Method: top-N heapsort  Memory: 48kB
#   Buffers: shared hit=4096 read=312
#   ->  Hash Join  (cost=420.00..1842.50 rows=512 width=64) (actual time=0.310..1.195 rows=512 loops=1)
#         Hash Cond: (o.customer_id = c.id)
#         Buffers: shared hit=4096 read=312
#         ->  Seq Scan on orders o  (cost=0.00..1350.00 rows=512 width=48) (actual time=0.015..0.890 rows=512 loops=1)
#               Filter: ((status = 'pending'::text) AND (created_at > '2026-01-01 00:00:00+00'::timestamp with time zone))
#               Rows Removed by Filter: 87000
#               Buffers: shared hit=3800 read=280
#         ->  Hash  (cost=270.00..270.00 rows=12000 width=20) (actual time=0.290..0.290 rows=12000 loops=1)
#               Buckets: 16384  Batches: 1  Memory Usage: 640kB
#               Buffers: shared hit=296 read=32
#               ->  Seq Scan on customers c  (cost=0.00..270.00 rows=12000 width=20) (actual time=0.005..0.150 rows=12000 loops=1)
#                     Buffers: shared hit=296 read=32
```

## Section-by-Section Breakdown

### Timestamp and Connection Identity

```
# Time: 2026-03-11T09:15:32.847291
# User@Host: app_user[app_user] @ 10.0.1.42 []
```

- **Time** -- ISO-8601 timestamp (microsecond precision) of when the log entry was written.
- **User@Host** -- The PostgreSQL role and the client's IP address, matching the MySQL slow log `User@Host` format so pt-query-digest can parse it.

These lines appear at **all** verbosity levels. No special GUCs beyond `peql.enabled` and `peql.log_min_duration` are needed.

### Connection Metadata

```
# Thread_id: 48712  Schema: mydb.public
# Bytes_sent: 16384
# Query_id: -6432758210044805760
```

- **Thread_id** -- The PostgreSQL backend PID (analogous to MySQL's thread ID).
- **Schema** -- Database and active schema in `db.schema` format, derived from `search_path`.
- **Bytes_sent** -- Estimated bytes sent to the client (planner row width times rows processed).
- **Query_id** -- The query fingerprint from PostgreSQL's `compute_query_id`. Useful for cross-referencing with `pg_stat_statements`.

Requires: **`peql.log_verbosity = 'standard'`** or higher. `Query_id` additionally needs PostgreSQL's `compute_query_id = on` (or `auto`).

### Core Timing and Row Counts

```
# Query_time: 1.285034  Lock_time: 0.000000  Rows_sent: 256  Rows_examined: 87500
# Rows_affected: 0
```

- **Query_time** -- Total execution wall-clock time in seconds.
- **Lock_time** -- Always `0.000000`; PostgreSQL does not expose per-query lock wait time the way MySQL does. Present for pt-query-digest format compatibility.
- **Rows_sent** -- Rows returned to the client (for SELECTs).
- **Rows_examined** -- Total tuples scanned across all plan nodes. At `full` verbosity this is derived from the plan tree; at lower levels it falls back to `Rows_sent`.
- **Rows_affected** -- Rows modified by DML (INSERT/UPDATE/DELETE). `0` for SELECTs.

`Query_time`, `Lock_time`, `Rows_sent`, and `Rows_examined` appear at **all** verbosity levels. `Rows_affected` requires `standard` or higher.

### Buffer I/O Metrics

```
# Shared_blks_hit: 4096  Shared_blks_read: 312  Shared_blks_dirtied: 0  Shared_blks_written: 0
# Local_blks_hit: 0  Local_blks_read: 0  Local_blks_dirtied: 0  Local_blks_written: 0
# Temp_blks_read: 0  Temp_blks_written: 48
# Shared_blk_read_time: 0.024310  Shared_blk_write_time: 0.000000
```

Per-query buffer usage deltas from PostgreSQL's `BufferUsage` counters:

- **Shared_blks_\*** -- Shared buffer pool activity (hits avoid disk I/O; reads go to the OS).
- **Local_blks_\*** -- Local buffer activity (temporary tables created with `ON COMMIT`).
- **Temp_blks_\*** -- Temp file activity (sorts, hash joins, CTEs that spill to disk).
- **Shared_blk_read_time / write_time** -- Wall-clock time spent on shared block I/O (seconds).

Requires: **`peql.log_verbosity = 'full'`**. The `read_time`/`write_time` fields additionally need PostgreSQL's `track_io_timing = on` and `peql.track_io_timing = on` (the latter defaults to `on`).

### WAL Metrics

```
# WAL_records: 0  WAL_bytes: 0  WAL_fpi: 0
```

Write-Ahead Log usage for this query:

- **WAL_records** -- Number of WAL records generated.
- **WAL_bytes** -- Total WAL bytes generated.
- **WAL_fpi** -- Full-page images written (happen after a checkpoint for pages modified for the first time).

A read-only SELECT produces zeros here; DML and DDL will show non-zero values.

Requires: **`peql.log_verbosity = 'full'`** and **`peql.track_wal = on`** (defaults to `on`).

### Planning Time

```
# Plan_time: 0.003210
```

Wall-clock time spent in the planner, measured separately from execution via a planner hook. Useful for identifying queries where planning dominates total latency (e.g., complex joins, many partitions).

Requires: **`peql.log_verbosity = 'full'`** and **`peql.track_planning = on`**.

### Plan Quality Indicators

```
# Full_scan: Yes  Temp_table: No  Temp_table_on_disk: Yes  Filesort: Yes  Filesort_on_disk: No
```

Boolean flags derived by walking the executed plan tree:

- **Full_scan** -- A `SeqScan` node was present (the query did a sequential/full table scan).
- **Temp_table** -- A `Material` node was present (intermediate results were materialized).
- **Temp_table_on_disk** -- Temp blocks were written to disk (`temp_blks_written > 0`).
- **Filesort** -- A `Sort` node was present.
- **Filesort_on_disk** -- The sort spilled to disk (checked via `TuplesortInstrumentation`).

These map directly to the MySQL slow log flags of the same name, letting pt-query-digest filter on them (e.g., `--filter '$event->{Full_scan} eq "Yes"'`).

Requires: **`peql.log_verbosity = 'full'`**.

### Per-Table I/O Attribution

```
# Table_io: public.orders (hit=3800 read=280), public.customers (hit=296 read=32)
```

Buffer hits and reads broken down by table, sorted by total I/O descending. Shows the top 5 tables. Helps pinpoint which tables are driving I/O for a given query without needing to look at the full plan.

Requires: **`peql.log_verbosity = 'full'`**.

### JIT Compilation Metrics

```
# JIT_functions: 4  JIT_generation_time: 0.001250  JIT_inlining_time: 0.000830  JIT_optimization_time: 0.002410  JIT_emission_time: 0.003100
```

If PostgreSQL's JIT compiler activated for this query:

- **JIT_functions** -- Number of functions JIT-compiled.
- **JIT_generation_time** -- Time generating LLVM IR (seconds).
- **JIT_inlining_time** -- Time inlining function bodies (seconds).
- **JIT_optimization_time** -- Time running LLVM optimization passes (seconds).
- **JIT_emission_time** -- Time emitting native machine code (seconds).

These lines only appear when JIT was actually used. PostgreSQL enables JIT automatically for expensive queries (controlled by `jit_above_cost`).

Requires: **`peql.log_verbosity = 'full'`** and PostgreSQL's JIT enabled (`jit = on`, which is the default).

### Memory Context Usage

```
# Mem_allocated: 2097152
```

Bytes allocated in the executor's per-query memory context (`es_query_cxt`) and all its children. This is block-level granularity -- it includes internal fragmentation and is typically higher than the actual bytes consumed by `palloc()` calls. See [Compatibility Notes](compatibility.md) for caveats.

Requires: **`peql.log_verbosity = 'full'`** and **`peql.track_memory = on`**.

### Wait Event Histogram

```
# Wait_events: IO:DataFileRead=12, Client:ClientRead=3
```

A histogram of wait events sampled during query execution at ~10 ms intervals. Each entry is `type:event=count`. Common examples:

- `IO:DataFileRead` -- Backend was waiting for a data file read from the OS.
- `Client:ClientRead` -- Backend was waiting for the client to send data.
- `LWLock:BufferMapping` -- Contention on the buffer mapping hash table.

Requires: **`peql.log_verbosity = 'full'`** and **`peql.track_wait_events = on`**.

### Rate Limit Metadata

```
# Log_slow_rate_type: query  Log_slow_rate_limit: 10  Log_slow_rate_limit_always_log_duration: 5000
```

When `peql.rate_limit > 1`, these fields record the sampling configuration so that pt-query-digest (or any post-processor) can extrapolate totals from sampled data:

- **Log_slow_rate_type** -- `session` (decide once per backend) or `query` (decide per statement).
- **Log_slow_rate_limit** -- The configured 1-in-N rate.
- **Log_slow_rate_limit_always_log_duration** -- Queries slower than this many milliseconds bypass the rate limiter entirely. `-1` means the override is disabled.

These lines only appear when `peql.rate_limit > 1`.

### SET timestamp and Query Text

```
SET timestamp=1741680931;
SELECT o.id, o.total, c.name
  FROM orders o
  JOIN customers c ON c.id = o.customer_id
 WHERE o.status = $1
   AND o.created_at > $2
 ORDER BY o.total DESC
 LIMIT 256;
```

- **SET timestamp** -- Unix epoch of the query's *start* time (not completion time). This is the standard MySQL slow log convention that pt-query-digest uses for time-window analysis.
- **Query text** -- The full SQL statement. Parameterized queries show `$1`, `$2`, etc. as placeholders; the actual values appear in the `# Parameters:` line below when `peql.log_parameter_values` is enabled.

These lines appear at **all** verbosity levels.

### Bind Parameter Values

```
# Parameters: $1 = 'pending', $2 = '2026-01-01 00:00:00+00'
```

The actual values bound to the query's parameter placeholders. Values are single-quoted, with internal single quotes escaped as `''` and special characters (`\n`, `\r`, `\\`) escaped. `NULL` values are shown unquoted.

Requires: **`peql.log_parameter_values = on`**.

### EXPLAIN ANALYZE Plan

```
# Plan:
# Sort  (cost=1850.23..1850.87 rows=256 width=64) (actual time=1.280..1.283 rows=256 loops=1)
#   Sort Key: o.total DESC
#   Sort Method: top-N heapsort  Memory: 48kB
#   Buffers: shared hit=4096 read=312
#   ->  Hash Join  (cost=420.00..1842.50 rows=512 width=64) (actual time=0.310..1.195 rows=512 loops=1)
#         ...
```

The full `EXPLAIN ANALYZE` output from the just-completed execution, prefixed with `# ` on every line so it doesn't interfere with pt-query-digest parsing. The plan includes:

- **Actual rows and timing** per node (`es->analyze = true`, `es->timing = true`).
- **Buffer counts** per node when verbosity is `full` (`es->buffers = true`).
- **WAL metrics** per node when `peql.track_wal = on` and verbosity is `full` (`es->wal = true`).

The format can be switched to JSON with `peql.log_query_plan_format = 'json'`.

Requires: **`peql.log_query_plan = on`**.

## Quick Reference: GUC to Feature Mapping

| Feature | Required GUCs |
|---------|---------------|
| Basic logging (Time, User@Host, Query_time, Rows) | `peql.enabled = on`, `peql.log_min_duration >= 0` |
| Thread_id, Schema, Bytes_sent, Rows_affected | `peql.log_verbosity = 'standard'` or `'full'` |
| Query_id | `peql.log_verbosity = 'standard'+` and `compute_query_id = on` |
| Buffer I/O counters | `peql.log_verbosity = 'full'` |
| Block read/write times | `peql.log_verbosity = 'full'`, `track_io_timing = on`, `peql.track_io_timing = on` |
| WAL metrics | `peql.log_verbosity = 'full'`, `peql.track_wal = on` |
| Planning time | `peql.log_verbosity = 'full'`, `peql.track_planning = on` |
| Plan quality flags | `peql.log_verbosity = 'full'` |
| Per-table I/O | `peql.log_verbosity = 'full'` |
| JIT metrics | `peql.log_verbosity = 'full'`, `jit = on` (PG default) |
| Memory tracking | `peql.log_verbosity = 'full'`, `peql.track_memory = on` |
| Wait event histogram | `peql.log_verbosity = 'full'`, `peql.track_wait_events = on` |
| Rate limit metadata | `peql.rate_limit > 1` |
| Bind parameter values | `peql.log_parameter_values = on` |
| EXPLAIN ANALYZE plan | `peql.log_query_plan = on` |
