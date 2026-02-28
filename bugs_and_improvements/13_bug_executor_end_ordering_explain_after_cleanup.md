# Bug: EXPLAIN plan generated after ExecutorEnd chains to cleanup

## Status: Fixed

## Priority: High

## Effort: Medium

## Description

In `peql_ExecutorEnd`, the log entry (including optional EXPLAIN output) is written
*before* chaining to `prev_ExecutorEnd` / `standard_ExecutorEnd`:

```c
if (msec >= peql_log_min_duration && peql_should_log(msec))
    peql_write_log_entry(queryDesc, msec);    /* calls ExplainPrintPlan */

if (prev_ExecutorEnd)
    prev_ExecutorEnd(queryDesc);
else
    standard_ExecutorEnd(queryDesc);
```

This ordering is correct for *this* extension in isolation -- the plan state is
still live when EXPLAIN runs. However, there is a subtle interaction problem:

If another extension (e.g., `pg_stat_statements`) has installed its own
`ExecutorEnd_hook` *before* this extension was loaded, then `prev_ExecutorEnd`
points to that other extension's hook. That hook was saved as the "previous" hook,
meaning it runs *after* our code. This is fine.

But if this extension is loaded *before* another extension that also hooks
`ExecutorEnd`, that other extension's hook becomes the outermost hook and calls
our `peql_ExecutorEnd` via its `prev_ExecutorEnd`. In that case, our code runs
first, and the ordering is still correct.

**The real problem is the EXPLAIN call itself**: `ExplainPrintPlan` calls
`ExplainNode`, which may call `show_sort_info`, `show_hash_info`, etc. These
functions access `tuplesortstate`, hash table internals, and other executor
state that is potentially being read at a point where the executor has already
called `InstrEndLoop` but has not fully torn down. If `peql_log_query_plan`
is enabled along with parallel query, the worker instrumentation may have
already been aggregated and the underlying structures freed by
`ExecParallelCleanup`, depending on the timing relative to `ExecEndPlan`.

In practice, `auto_explain` does the same thing (calls ExplainPrintPlan from
ExecutorEnd_hook before chaining), so this is a known-safe pattern for the
*non-parallel* case. But with parallel workers, the safety depends on the
exact PostgreSQL version and whether `ExecParallelCleanup` has run.

## Location

`pg_enhanced_query_logging.c`, lines 673-694 (`peql_ExecutorEnd`) and
lines 1152-1176 (`peql_format_entry`, EXPLAIN block)

## Fix

For the non-parallel case, the current ordering is correct and mirrors
`auto_explain`. For robustness with parallel workers:

1. Wrap the `ExplainPrintPlan` call in a `PG_TRY` block so a crash in
   plan display doesn't bring down the backend.
2. Consider checking `queryDesc->estate->es_use_parallel_mode` and skipping
   the detailed plan output (or at least the worker instrumentation) when
   parallel execution was used, unless we can confirm that `es_jit_worker_instr`
   and other parallel structures are still valid.
