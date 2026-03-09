# Bug: Stats counters are per-backend, always return zero from a different session

## Status: Fixed

## Priority: High

## Effort: Low

## Description

The `pg_enhanced_query_logging_stats()` function returned `queries_logged`,
`queries_skipped`, and `bytes_written` from `static int64` variables local to
each backend process. Since each PostgreSQL backend has its own copy of these
counters, querying the stats from a different session (e.g. a `psql` session
after a pgbench run) always returned zeros -- the calling backend had never
logged anything itself.

This made the stats function effectively useless for monitoring or benchmarking,
even though the log file on disk clearly showed data being written.

```c
static int64 peql_queries_logged  = 0;
static int64 peql_queries_skipped = 0;
static int64 peql_bytes_written   = 0;
```

The `pg_enhanced_query_logging_reset()` function had the same problem: it only
zeroed the calling backend's counters, not the counters of the backends that
actually performed the logging.

## Location

`pg_enhanced_query_logging.c`:
- Static counter declarations (formerly lines 185-187)
- Increment sites in `peql_write_log_entry`, `peql_write_utility_log_entry`,
  executor hook, and utility hook
- `pg_enhanced_query_logging_reset()` zero assignments
- `pg_enhanced_query_logging_stats()` return values

## Fix

Moved the three counters into the existing `PeqlSharedState` struct in shared
memory as `pg_atomic_uint64` fields (`total_queries_logged`,
`total_queries_skipped`, `total_bytes_written`). This follows the same lock-free
atomic pattern already used by the adaptive rate limiter's `queries_this_window`
and `bytes_this_window` counters in the same struct.

All increment sites now use `pg_atomic_fetch_add_u64`, the reset function uses
`pg_atomic_write_u64`, and the stats function uses `pg_atomic_read_u64`. No
locks are needed -- hardware-level atomics provide correct concurrent access
with negligible overhead (a single atomic add per logged query on the hot path).
