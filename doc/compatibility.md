# Compatibility Notes

[Back to README](../README.md)

- **Lock_time** is always reported as `0.000000`. PostgreSQL does not expose per-query lock acquisition time in the same way MySQL does. The field is present for pt-query-digest format compatibility.
- **Rows_examined** at `full` verbosity reflects actual tuples processed by scan nodes in the plan tree. At lower verbosity levels it falls back to `Rows_sent` for SELECTs.
- **Buffer and WAL metrics** are per-query executor deltas (zero-initialized per query via `InstrAlloc`). Buffer I/O during `ExecutorStart` (e.g., catalog lookups for plan initialization) is not captured, matching the behavior of `auto_explain` and `pg_stat_statements`. The practical impact is minimal.
- **I/O timing metrics** require PostgreSQL's `track_io_timing = on` to be set at the server level for the underlying counters to be populated.
- **Memory tracking** (`peql.track_memory`) is experimental and adds overhead:
  - **Block-level granularity**: `MemoryContextMemAllocated()` reports bytes allocated to memory context *blocks*, not the bytes consumed by individual `palloc()` calls. PostgreSQL's allocator requests memory in blocks and hands out smaller pieces from within them, so the reported number includes internal fragmentation and is typically higher than the actual memory the query consumed.
  - **Partial coverage**: Only the executor's per-query context (`es_query_cxt`) and its children are measured. Memory allocated elsewhere -- by the planner, in the transaction context, or in shared/cached catalog structures -- is not included. The number is a useful indicator of executor memory pressure but not a total "memory used by this query" figure.
  - **Traversal cost**: The measurement walks the entire tree of child memory contexts under `es_query_cxt` (recursive mode). For queries with many sub-contexts (complex plans, hash aggregates, CTEs), this traversal adds overhead proportional to the number of contexts created.
- **Nested query timing**: When `peql.log_nested = on`, nested queries inside PL/pgSQL functions are logged individually. The enclosing query's `Query_time` includes time spent in nested queries, so the same execution time may appear in multiple log entries. pt-query-digest reports will show inflated total time when nested logging is enabled. To avoid double-counting, use `peql.log_nested = off` (the default) or post-process the log to exclude nested entries.
- The extension coexists with `pg_stat_statements`, `auto_explain`, and other hook-based extensions through proper hook chaining.
