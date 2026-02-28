# Bug: Cursor fetches and SPI queries can cause duplicate or misleading log entries

## Status: Fixed

## Priority: Medium

## Effort: Medium

## Description

When a query uses cursors (either explicitly via DECLARE/FETCH or implicitly
via PL/pgSQL FOR loops), the executor pipeline is invoked multiple times for
the same QueryDesc:

1. `ExecutorStart` is called once.
2. `ExecutorRun` is called multiple times (once per FETCH).
3. `ExecutorEnd` is called once.

The `nesting_level` tracking increments/decrements around each `ExecutorRun`
call, which is correct for nesting detection. However, the instrumentation
issue is subtler:

- `queryDesc->totaltime` accumulates across all `ExecutorRun` calls (each
  Start/EndLoop pair adds to `total`).
- The `es_processed` row count also accumulates.

So the single log entry written at `ExecutorEnd` correctly captures the full
execution time. **This is fine for cursors.**

However, for **SPI (Server Programming Interface)** calls from PL/pgSQL:

- Each SPI query in a function gets its own `ExecutorStart` -> `ExecutorRun`
  -> `ExecutorEnd` cycle.
- With `peql.log_nested = on`, each individual SPI query is logged separately.
- But the outer function's query (which triggered the PL/pgSQL execution) is
  also logged at the top level.
- The outer query's `totaltime` includes the wall-clock time spent in the
  function, which includes the time of all inner SPI queries.

This means: with `log_nested = on`, the inner queries' times are logged
individually AND included in the outer query's time. A pt-query-digest report
will show inflated total query time because the same seconds are counted
multiple times. This is a well-known problem shared with `auto_explain` and
`log_min_duration_statement`, but it should be documented.

## Location

`pg_enhanced_query_logging.c`, lines 574-615 (ExecutorRun/Finish hooks)
and lines 673-694 (ExecutorEnd hook)

## Fix

This is not a code fix but a documentation issue. Add a note to the README
under "Compatibility Notes":

```
When `peql.log_nested = on`, nested queries inside PL/pgSQL functions are
logged individually. The enclosing query's `Query_time` includes the time
spent in nested queries. This means the same execution time may appear in
multiple log entries. pt-query-digest reports will show inflated total
time when nested logging is enabled. To avoid double-counting, either use
`peql.log_nested = off` (the default) or post-process the log to exclude
nested entries.
```
