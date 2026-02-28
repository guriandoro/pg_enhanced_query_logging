# Bug: StringInfo memory leak if formatting or file I/O throws an error

## Status: Fixed

## Priority: Medium

## Effort: Easy

## Description

Both `peql_write_log_entry` and `peql_write_utility_log_entry` allocate a
`StringInfo` buffer in `CurrentMemoryContext`, format the log entry, write it,
then `pfree` the buffer:

```c
static void
peql_write_log_entry(QueryDesc *queryDesc, double duration_ms)
{
    StringInfoData buf;

    initStringInfo(&buf);
    peql_format_entry(&buf, queryDesc, duration_ms);
    peql_flush_to_file(buf.data, buf.len);
    pfree(buf.data);
    ...
}
```

If `peql_format_entry` throws an error (e.g., from `get_namespace_name`,
`getTypeOutputInfo`, `OidOutputFunctionCall` in parameter formatting, or
`ExplainPrintPlan`), the `pfree` is never reached and the buffer leaks.

More importantly, the error propagates up through `peql_ExecutorEnd`, which
does not have its own `PG_TRY` block. This means:

1. The original query's error handling is disrupted by a secondary error from
   our formatting code.
2. If the formatting error is different from the original query error (e.g.,
   the query succeeded but parameter formatting fails), the user sees an
   unexpected error from the logging extension.
3. The leaked StringInfo buffer accumulates over the transaction's lifetime.

## Location

`pg_enhanced_query_logging.c`, lines 1381-1393 (`peql_write_log_entry`) and
lines 1398-1408 (`peql_write_utility_log_entry`)

## Fix

Wrap the format + write + free sequence in a `PG_TRY` / `PG_CATCH` block that
ensures cleanup and suppresses errors from the logging path, so a logging
failure never disrupts the user's query:

```c
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
    }
    PG_CATCH();
    {
        /* Swallow the error -- logging must not break queries. */
        FlushErrorState();
        ereport(LOG,
                (errmsg("peql: error while writing log entry, skipping")));
    }
    PG_END_TRY();

    MemoryContextSwitchTo(oldcxt);
    MemoryContextDelete(logcxt);

    peql_current_plan_time_ms = 0.0;
}
```

Using a dedicated memory context ensures all allocations are cleaned up
regardless of whether formatting succeeds or throws.
