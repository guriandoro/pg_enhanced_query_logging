# SQL Functions

[Back to README](../README.md)

## `pg_enhanced_query_logging_reset()`

Rotates the slow query log file (renames to `.old`) and resets the global statistics counters. Requires superuser privileges.

```sql
SELECT pg_enhanced_query_logging_reset();
-- NOTICE: peql: log file "/path/to/peql-slow.log" has been rotated to "...peql-slow.log.old"
-- Returns: true
```

## `pg_enhanced_query_logging_stats()`

Returns global logging counters aggregated across all backends via shared memory. The counters reset on log rotation (`pg_enhanced_query_logging_reset()`) or server restart. Requires superuser privileges.

| Column | Type | Description |
|--------|------|-------------|
| `queries_logged` | bigint | Total log entries written since last reset or server start |
| `queries_skipped` | bigint | Queries that exceeded the duration threshold but were suppressed by rate limiting |
| `bytes_written` | bigint | Total bytes appended to the log file |

```sql
SELECT * FROM pg_enhanced_query_logging_stats();
 queries_logged | queries_skipped | bytes_written
----------------+-----------------+---------------
            142 |               8 |         48320
(1 row)
```
