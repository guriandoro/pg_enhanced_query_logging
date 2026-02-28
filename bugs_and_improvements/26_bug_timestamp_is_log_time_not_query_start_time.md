# Bug: `SET timestamp` and `# Time:` reflect log-write time, not query start time

## Status: Fixed

## Priority: Medium

## Effort: Easy

## Description

The timestamp in the log entry is computed in `peql_format_entry`:

```c
now = GetCurrentTimestamp();
stamp_time = timestamptz_to_time_t(now);
```

This captures the time when the log entry is *being formatted*, which happens
in `ExecutorEnd` -- i.e., **after** the query has finished executing. For a
query that takes 10 seconds, the `# Time:` line shows a timestamp 10 seconds
after the query actually started.

In contrast, MySQL's slow query log `# Time:` line records the time the query
*completed* (which matches what we do), but the `SET timestamp=` line records
the time the query *started*. pt-query-digest uses `SET timestamp` as the
query start time for its time-based grouping and histogram.

The current code uses the same timestamp for both fields:

```c
appendStringInfo(buf, "# Time: %s\n", timebuf);  /* completion time */
...
appendStringInfo(buf, "SET timestamp=%ld;\n", (long) stamp_time);  /* also completion time */
```

For accurate timeline reconstruction with pt-query-digest, `SET timestamp`
should be `completion_time - query_duration` (the query start time).

## Location

`pg_enhanced_query_logging.c`, lines 920-938 and 1138 (`peql_format_entry`)
Same issue at lines 1322-1339 and 1362 (`peql_format_utility_entry`)

## Fix

Compute the query start time by subtracting the duration from the current
timestamp:

```c
now = GetCurrentTimestamp();
stamp_time = timestamptz_to_time_t(now);

/* SET timestamp should reflect query *start* time, not completion time. */
pg_time_t start_time = stamp_time - (pg_time_t)(duration_ms / 1000.0);
...
appendStringInfo(buf, "SET timestamp=" INT64_FORMAT ";\n", (int64) start_time);
```

Keep `# Time:` as the completion timestamp (this matches MySQL's convention).
