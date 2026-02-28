# Bug: Per-node plan instrumentation never enabled, making Rows_examined always 0

## Status: Not fixed

## Priority: Critical

## Effort: Easy

## Description

At `full` verbosity, the plan walker tries to collect `rows_examined` by
reading `planstate->instrument->ntuples` from each plan node. However,
`planstate->instrument` is always `NULL` because per-node instrumentation
is never enabled.

Per-node instrumentation is controlled by `queryDesc->instrument_options`,
which `ExecInitNode` reads during `standard_ExecutorStart` to decide whether
to call `InstrAlloc` on each node. The extension's `peql_ExecutorStart` calls
`standard_ExecutorStart` **first**, then allocates `queryDesc->totaltime`
**after**:

```c
if (prev_ExecutorStart)
    prev_ExecutorStart(queryDesc, eflags);    /* nodes built here */
else
    standard_ExecutorStart(queryDesc, eflags); /* nodes built here */

/* Too late -- nodes are already initialized without instrumentation */
if (peql_active())
{
    if (queryDesc->totaltime == NULL)
        queryDesc->totaltime = InstrAlloc(1, INSTRUMENT_ALL, false);
}
```

`queryDesc->instrument_options` is never set to `INSTRUMENT_ALL`, so
`ExecInitNode` creates plan state nodes without `Instrumentation` structs.
The `totaltime` allocation only affects the top-level execution timer, not
individual nodes.

This means:
1. `Rows_examined` is always 0 at `full` verbosity (plan walker finds no
   instrumented nodes).
2. `Full_scan`, `Filesort`, `Filesort_on_disk` detection works (those use
   `IsA()` node type checks, not instrumentation data), but sort-spill
   detection via `tuplesort_get_stats` may also be affected if the sort
   state isn't fully populated without instrumentation.
3. At `minimal`/`standard` verbosity, `Rows_examined` falls back to
   `rows_processed` for SELECTs (which equals `Rows_sent`) and `0` for DML
   -- so SELECTs show a value but it's not actually "examined" rows.

## Location

`pg_enhanced_query_logging.c`, lines 539-564 (`peql_ExecutorStart`)

## Fix

Set `queryDesc->instrument_options` **before** calling `standard_ExecutorStart`
so that `ExecInitNode` allocates per-node instrumentation:

```c
static void
peql_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
    if (peql_active())
    {
        queryDesc->instrument_options |= INSTRUMENT_ALL;
    }

    if (prev_ExecutorStart)
        prev_ExecutorStart(queryDesc, eflags);
    else
        standard_ExecutorStart(queryDesc, eflags);

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
```

This is exactly what `auto_explain` does -- it sets `instrument_options`
before the standard start so every node gets instrumentation.

Note: enabling `INSTRUMENT_ALL` on every node adds some overhead (timing
calls on every node start/end). This is acceptable when the extension is
active, but the cost should be documented. To minimize overhead when only
minimal/standard verbosity is needed, the instrumentation could be
conditioned on `peql_log_verbosity >= PEQL_LOG_VERBOSITY_FULL`.
